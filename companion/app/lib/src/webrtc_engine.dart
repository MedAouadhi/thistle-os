/// The call media engine over flutter_webrtc and its prebuilt libwebrtc
/// (ADR-0016), for calls between companion apps. Ticket 02 of
/// .scratch/tailnet-video settled how: no ICE server and no TCP candidate,
/// one bundled transport (`max-bundle`, `rtcp-mux` required), VP8 then the
/// rest without H.264 for video, Opus first for audio, and the pair in use
/// read back from getStats. libwebrtc's own echo canceller does the work
/// the PipeWire one does for a `ws-opus` call.
///
/// The video transceiver is negotiated from the start with nothing on it;
/// the camera's track replaces nothing, or another camera's, and is
/// replaced by nothing when it goes off. A camera that opens without error
/// can still give no frame (issue 2225), so its first frame is waited for.
library;

import 'dart:async';

import 'package:flutter/widgets.dart';
import 'package:flutter_webrtc/flutter_webrtc.dart';

import 'call_engine.dart';

class WebrtcCallEngine implements CallEngine {
  WebrtcCallEngine({this.log});
  final void Function(String)? log;

  /// No STUN, no TURN: the tailnet's own addresses are the only path.
  static const Map<String, dynamic> configuration = {
    'iceServers': <dynamic>[],
    'iceTransportPolicy': 'all',
    'bundlePolicy': 'max-bundle',
    'rtcpMuxPolicy': 'require',
    'tcpCandidatePolicy': 'disabled',
    'sdpSemantics': 'unified-plan',
    'iceCandidatePoolSize': 0,
  };

  /// How long a camera has to give its first frame.
  static const firstFrame = Duration(milliseconds: 2500);

  @override
  Future<List<String>> cameras() async {
    final devices = await navigator.mediaDevices.enumerateDevices();
    final found = [
      for (final d in devices)
        if (d.kind == 'videoinput') d,
    ];
    log?.call('cameras: ${found.isEmpty ? 'none' : found.map((d) => d.label).join(', ')}');
    return [for (final d in found) d.deviceId];
  }

  @override
  Future<CallSession> start({
    required bool offerer,
    required SendRtc send,
    required void Function(String state) onState,
  }) async {
    // getUserMedia holds the UI thread for about a second on Linux (ticket
    // 02): let the call screen draw first, so the stall lands on no tap.
    // Without frames (no window yet, a test) it goes on after a moment.
    await _drawn();
    final mic = await navigator.mediaDevices.getUserMedia({'audio': true, 'video': false});
    final RTCPeerConnection pc;
    try {
      pc = await createPeerConnection(configuration);
    } on Object {
      await _stop(mic);
      rethrow;
    }
    final s = _Session(pc, mic, send, onState, log);
    try {
      await s._open(offerer);
    } on Object {
      await s.close();
      rethrow;
    }
    return s;
  }
}

Future<void> _drawn() =>
    WidgetsBinding.instance.endOfFrame.timeout(const Duration(milliseconds: 250), onTimeout: () {});

Future<void> _stop(MediaStream stream) async {
  for (final t in stream.getTracks()) {
    await t.stop();
  }
  await stream.dispose();
}

class _Session implements CallSession {
  _Session(this.pc, this.mic, this._send, this._onState, this.log);
  final RTCPeerConnection pc;
  final MediaStream mic;
  final SendRtc _send;
  final void Function(String) _onState;
  final void Function(String)? log;

  /// Our candidates wait until our SDP has gone, and theirs until theirs is
  /// set: either may otherwise reach a connection with no description.
  bool _localSent = false, _remoteSet = false, _closed = false;
  final List<RTCIceCandidate> _ours = [], _theirs = [];
  MediaStreamTrack? _remoteAudio;
  double _volume = 1;

  /// The video transceiver's sender, the camera's stream on it, and what
  /// draws each side's picture.
  RTCRtpSender? _video;
  MediaStream? _camera;
  final _remote = RTCVideoRenderer(), _local = RTCVideoRenderer();
  MediaStream? _remoteStream;

  Future<void> _open(bool offerer) async {
    await _remote.initialize();
    await _local.initialize();
    pc.onIceCandidate = (c) {
      if ((c.candidate ?? '').isEmpty) return; // the end of gathering
      _localSent ? _sendCandidate(c) : _ours.add(c);
    };
    pc.onIceConnectionState = (state) {
      final s = switch (state) {
        RTCIceConnectionState.RTCIceConnectionStateConnected ||
        RTCIceConnectionState.RTCIceConnectionStateCompleted => 'connected',
        RTCIceConnectionState.RTCIceConnectionStateDisconnected => 'disconnected',
        RTCIceConnectionState.RTCIceConnectionStateFailed => 'failed',
        _ => null,
      };
      if (s != null && !_closed) _onState(s);
    };
    pc.onTrack = (e) {
      if (e.track.kind == 'video') {
        unawaited(_showRemote(e));
        return;
      }
      if (e.track.kind != 'audio') return;
      _remoteAudio = e.track;
      if (_volume != 1) unawaited(_applyVolume());
    };
    for (final t in mic.getAudioTracks()) {
      await pc.addTrack(t, mic);
    }
    if (!offerer) return; // the answer's transceivers come with the offer
    // The camera is off, but a video track is negotiated from the start,
    // both ways, so switching it on never renegotiates.
    final video = await pc.addTransceiver(
      kind: RTCRtpMediaType.RTCRtpMediaTypeVideo,
      init: RTCRtpTransceiverInit(direction: TransceiverDirection.SendRecv),
    );
    _video = video.sender;
    await _preferCodecs();
    final offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    _sendDescription(offer);
  }

  /// The other side's video track, on a stream of its own if it came with
  /// none (a transceiver that had no track when it was negotiated).
  Future<void> _showRemote(RTCTrackEvent e) async {
    if (_closed) return;
    var stream = e.streams.firstOrNull;
    if (stream == null) {
      stream = _remoteStream ??= await createLocalMediaStream('remote');
      await stream.addTrack(e.track);
    }
    _remote.srcObject = stream;
  }

  void _sendDescription(RTCSessionDescription d) {
    _send(sdp: {'type': d.type, 'sdp': d.sdp});
    _localSent = true;
    for (final c in _ours) {
      _sendCandidate(c);
    }
    _ours.clear();
  }

  void _sendCandidate(RTCIceCandidate c) =>
      _send(candidate: {'candidate': c.candidate, 'sdpMid': c.sdpMid, 'sdpMLineIndex': c.sdpMLineIndex});

  /// VP8 first and no H.264 for video, Opus first for audio.
  Future<void> _preferCodecs() async {
    for (final t in await pc.getTransceivers()) {
      final kind = t.receiver.track?.kind ?? t.sender.track?.kind ?? 'video';
      final codecs = (await getRtpSenderCapabilities(kind)).codecs ?? [];
      String mime(RTCRtpCodecCapability c) => c.mimeType.toLowerCase();
      final first = kind == 'video' ? 'video/vp8' : 'audio/opus';
      final order = [
        ...codecs.where((c) => mime(c) == first),
        ...codecs.where((c) => mime(c) != first && mime(c) != 'video/h264'),
      ];
      try {
        await t.setCodecPreferences(order);
      } on Object catch (e) {
        log?.call('WebRTC: codec preferences for $kind not set: $e');
      }
    }
  }

  @override
  String get echoCancellation => "on (WebRTC's own canceller, in the app)";

  @override
  Future<void> take({Map<String, Object?>? sdp, Map<String, Object?>? candidate}) async {
    if (_closed) return;
    if (sdp != null) {
      await pc.setRemoteDescription(RTCSessionDescription(sdp['sdp'] as String?, sdp['type'] as String?));
      _remoteSet = true;
      for (final c in _theirs) {
        await pc.addCandidate(c);
      }
      _theirs.clear();
      if (sdp['type'] == 'offer') {
        for (final t in await pc.getTransceivers()) {
          if (t.receiver.track?.kind != 'video') continue;
          await t.setDirection(TransceiverDirection.SendRecv);
          _video = t.sender;
        }
        await _preferCodecs();
        final answer = await pc.createAnswer();
        await pc.setLocalDescription(answer);
        _sendDescription(answer);
      }
    }
    if (candidate != null) {
      final c = RTCIceCandidate(
        candidate['candidate'] as String?,
        candidate['sdpMid'] as String?,
        candidate['sdpMLineIndex'] as int?,
      );
      _remoteSet ? await pc.addCandidate(c) : _theirs.add(c);
    }
  }

  @override
  set muted(bool on) {
    for (final t in mic.getAudioTracks()) {
      t.enabled = !on;
    }
  }

  @override
  set volume(double v) {
    _volume = v;
    unawaited(_applyVolume());
  }

  Future<void> _applyVolume() async {
    final track = _remoteAudio;
    if (track == null) return;
    try {
      await Helper.setVolume(_volume, track);
    } on Object catch (e) {
      log?.call('WebRTC: volume not set: $e');
    }
  }

  @override
  Future<void> setCamera(String? camera) async {
    if (_closed) return;
    final sender = _video ?? (throw const CameraUnavailable('the call has no video track yet'));
    final old = _camera;
    _camera = null;
    _local.srcObject = null;
    if (camera == null) {
      await sender.replaceTrack(null);
      if (old != null) await _stop(old);
      return;
    }
    // Another camera: the one in use is let go first, as a phone's front
    // and back cameras may not be open together.
    if (old != null) {
      await sender.replaceTrack(null);
      await _stop(old);
    }
    await _drawn();
    final MediaStream stream;
    try {
      stream = await navigator.mediaDevices.getUserMedia({
        'audio': false,
        'video': {
          'deviceId': camera,
          'optional': [
            {'sourceId': camera},
          ],
          'width': {'ideal': maxVideoWidth, 'max': maxVideoWidth},
          'height': {'ideal': maxVideoHeight, 'max': maxVideoHeight},
          'frameRate': {'ideal': maxVideoFps, 'max': maxVideoFps},
        },
      });
    } on Object catch (e) {
      throw CameraUnavailable('it did not open: $e');
    }
    final track = stream.getVideoTracks().firstOrNull;
    if (track == null || _closed) {
      await _stop(stream);
      if (track == null) throw const CameraUnavailable('it did not open');
      return;
    }
    // getUserMedia succeeds on a camera another process holds, which then
    // gives nothing: its first frame, drawn into our own picture, tells.
    final frame = Completer<void>();
    _local.onFirstFrameRendered = () {
      if (!frame.isCompleted) frame.complete();
    };
    _local.srcObject = stream;
    final got = await frame.future.then((_) => true).timeout(WebrtcCallEngine.firstFrame, onTimeout: () => false);
    _local.onFirstFrameRendered = null;
    if (!got || _closed) {
      _local.srcObject = null;
      await _stop(stream);
      if (!got) throw const CameraUnavailable('no picture came from it');
      return;
    }
    await sender.replaceTrack(track);
    _camera = stream;
    await _capSending(sender);
    final s = track.getSettings();
    log?.call('camera on: ${s['width']}x${s['height']} at ${s['frameRate']} fps');
  }

  /// At most [maxVideoFps] frames a second sent; the size is the capture's,
  /// at most 1280x720.
  Future<void> _capSending(RTCRtpSender sender) async {
    try {
      final p = sender.parameters;
      final encodings = p.encodings ?? [];
      if (encodings.isEmpty) encodings.add(RTCRtpEncoding());
      for (final e in encodings) {
        e.maxFramerate = maxVideoFps;
      }
      p.encodings = encodings;
      await sender.setParameters(p);
    } on Object catch (e) {
      log?.call('WebRTC: the sending cap was not set: $e');
    }
  }

  @override
  Future<VideoStats?> videoStats() async {
    if (_closed) return null;
    Map<dynamic, dynamic>? out, into;
    for (final r in await pc.getStats()) {
      final kind = r.values['kind'] ?? r.values['mediaType'];
      if (kind != 'video') continue;
      if (r.type == 'outbound-rtp') out = r.values;
      if (r.type == 'inbound-rtp') into = r.values;
    }
    String? size(Map<dynamic, dynamic>? v) =>
        v?['frameWidth'] == null ? null : '${v!['frameWidth']}x${v['frameHeight']}';
    return VideoStats(
      framesSent: (out?['framesSent'] as num?)?.toInt() ?? 0,
      framesReceived: (into?['framesReceived'] as num?)?.toInt() ?? (into?['framesDecoded'] as num?)?.toInt() ?? 0,
      sent: size(out),
      received: size(into),
      fpsSent: out?['framesPerSecond'] as num?,
      limitedBy: out?['qualityLimitationReason'] as String?,
    );
  }

  @override
  Widget remoteVideo() => RTCVideoView(_remote, objectFit: RTCVideoViewObjectFit.RTCVideoViewObjectFitCover);

  @override
  Widget localVideo() =>
      RTCVideoView(_local, objectFit: RTCVideoViewObjectFit.RTCVideoViewObjectFitCover, mirror: true);

  @override
  Future<CallPath?> selectedPath() async {
    final reports = await pc.getStats();
    final byId = {for (final r in reports) r.id: r.values};
    Map<dynamic, dynamic>? pair;
    for (final r in reports) {
      final id = r.values['selectedCandidatePairId'];
      if (r.type == 'transport' && id != null) pair = byId[id];
    }
    // Without a transport report, the nominated pair that works.
    pair ??= reports
        .where((r) => r.type == 'candidate-pair' && r.values['nominated'] == true && r.values['state'] == 'succeeded')
        .map((r) => r.values)
        .firstOrNull;
    if (pair == null) return null;
    final local = byId[pair['localCandidateId']], remote = byId[pair['remoteCandidateId']];
    if (local == null || remote == null) return null;
    return CallPath(
      local: '${local['address'] ?? local['ip']}',
      remote: '${remote['address'] ?? remote['ip']}',
      localType: '${local['candidateType']}/${local['protocol']}',
      remoteType: '${remote['candidateType']}/${remote['protocol']}',
    );
  }

  @override
  Future<void> close() async {
    if (_closed) return;
    _closed = true;
    _local.srcObject = null;
    _remote.srcObject = null;
    await pc.close();
    await _stop(mic);
    final camera = _camera;
    _camera = null;
    if (camera != null) await _stop(camera);
    await _remoteStream?.dispose();
    await pc.dispose();
    await _local.dispose();
    await _remote.dispose();
  }
}
