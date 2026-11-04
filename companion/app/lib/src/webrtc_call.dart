/// One WebRTC call's media, as the call on screen drives it: the engine's
/// session, the other side's SDP and candidates queued until it starts, our
/// camera, and the check that the media stays on the tailnet. Talk owns one
/// for a call between companion apps; a `ws-opus` call has none. Its SDP
/// and candidates go through the core as `call_rtc`, which keeps the
/// tailnet's alone.
library;

import 'dart:async';

import 'package:flutter/widgets.dart' show Widget;
import 'package:tailnet_chat_core/tailnet_chat_core.dart';

import 'call_engine.dart';
import 'child_processes.dart';

class WebrtcCall {
  WebrtcCall({
    required this.id,
    required this.engine,
    required this.peer,
    required this.children,
    required this.changed,
    bool camera = false,
    this.log,
  }) : _wantCamera = camera;

  final String id;
  final CallEngine engine;
  final ChatPeer Function() peer;

  /// None starts while our camera is on.
  final ChildProcesses children;

  /// Something on screen may have changed.
  final void Function() changed;
  final void Function(String)? log;

  /// The media, once started; the other side's SDP and candidates wait in
  /// `_waiting` until then, in order.
  CallSession? _session;
  bool _starting = false, _closed = false;
  final List<RtcPayload> _waiting = [];
  Future<void> _taken = Future.value();
  bool _muted = false;
  double _volume = 1;

  Call? get _call => peer().calls[id];
  bool get _live => !_closed && (_call?.live ?? false);

  /// Why the call has no audio, if WebRTC did not start.
  String? audioProblem;

  /// What removes the speaker from the microphone, once started.
  String? get echoCancellation => _session?.echoCancellation;

  set muted(bool on) {
    _muted = on;
    _session?.muted = on;
  }

  set volume(double v) {
    _volume = v;
    _session?.volume = v;
  }

  /// The other side's SDP or candidate (the core's callRtc).
  void take(RtcPayload payload) {
    final session = _session;
    session == null ? _waiting.add(payload) : _take(session, payload);
  }

  /// One at a time, in the order they came: a candidate never reaches the
  /// connection before the SDP it belongs to.
  void _take(CallSession session, RtcPayload payload) =>
      _taken = _taken.then((_) => session.take(payload)).catchError((Object e) {
        log?.call('${callLabel(id)}: WebRTC refused what the other side sent: $e');
      });

  /// Start the media, once the call is connected; again does nothing.
  Future<void> start() async {
    final c = _call;
    if (_session != null || _starting || c == null) return;
    _starting = true;
    log?.call('${callLabel(id)} over $transportWebrtc');
    final CallSession session;
    try {
      session = await engine.start(
        offerer: c.mine,
        send: (payload) => peer().sendRtc(id, payload),
        onState: _onMediaState,
      );
    } on Object catch (e) {
      _starting = false;
      audioProblem = 'The call has no audio: $e';
      log?.call('${callLabel(id)}: WebRTC did not start: $e');
      if (_live) peer().endCall(id, reason: 'error');
      changed();
      return;
    }
    _starting = false;
    if (!_live) {
      await session.close();
      return;
    }
    _session = session
      ..muted = _muted
      ..volume = _volume;
    for (final p in _waiting) {
      _take(session, p);
    }
    _waiting.clear();
    List<String> found;
    try {
      found = await engine.cameras();
    } on Object catch (e) {
      log?.call('${callLabel(id)}: no camera found: $e');
      found = const [];
    }
    if (_closed) return;
    cameras = found;
    // Said at the answer, as every change will be.
    if (found.isEmpty) {
      peer().setCamera(id, CameraState.none);
    } else if (_wantCamera) {
      await _applyCamera(session, on: true);
    } else {
      peer().setCamera(id, CameraState.off);
    }
    changed();
  }

  // -- the camera -----------------------------------------------------------------

  /// What our camera should be until the media starts.
  bool _wantCamera;

  /// This machine's cameras, once the media has started; empty with none.
  List<String>? cameras;
  int _cameraAt = 0;

  /// Whether our camera is sending.
  bool cameraOn = false;

  /// Why the camera is off when it was asked on.
  String? cameraProblem;

  bool get canSwitchCamera => (cameras?.length ?? 0) > 1;

  /// Whether the camera switch shows on: sending, or asked for before the
  /// media has started (Video call, while it rings).
  bool get cameraSwitchOn => _session == null ? _wantCamera : cameraOn;

  /// Whether the other side's video is to be drawn.
  bool get remoteVideoShown => _session != null && _call?.remoteCamera == CameraState.on;

  Widget? remoteVideo() => _session?.remoteVideo();
  Widget? localVideo() => cameraOn ? _session?.localVideo() : null;

  /// The video's frames so far.
  Future<VideoStats?> videoStats() async => _session?.videoStats();

  /// Our camera on or off: before the media starts, as it starts.
  Future<void> setCameraOn(bool on) async {
    final session = _session;
    cameraProblem = null;
    if (session == null) {
      _wantCamera = on;
      changed();
      return;
    }
    if (on == cameraOn || (on && (cameras ?? const []).isEmpty)) return;
    await _applyCamera(session, on: on);
    changed();
  }

  /// The next camera, round and back to the first.
  Future<void> switchCamera() async {
    final session = _session, list = cameras;
    if (session == null || list == null || list.length < 2 || !cameraOn) return;
    _cameraAt = (_cameraAt + 1) % list.length;
    await _applyCamera(session, on: true);
    changed();
  }

  /// No child process starts while the camera is on, and the camera waits
  /// for those running (child_processes.dart); a camera that gives no
  /// picture is turned off, said so, and told as none: the other side's
  /// tile reads No camera, not Camera off.
  Future<void> _applyCamera(CallSession session, {required bool on}) async {
    final camera = on ? cameras![_cameraAt] : null;
    if (on) await children.cameraOpening();
    var state = on ? CameraState.on : CameraState.off;
    try {
      await session.setCamera(camera);
      cameraOn = on;
    } on CameraUnavailable catch (e) {
      cameraOn = false;
      state = CameraState.none;
      cameraProblem = 'The camera is off: $e. Another program may be using it.';
      log?.call('${callLabel(id)}: camera $camera gave no picture: $e');
    }
    if (!cameraOn) children.cameraClosed();
    if (_closed) return;
    log?.call('${callLabel(id)}: our camera ${cameraOn ? 'on, $camera' : state.wire}');
    peer().setCamera(id, state);
  }

  // -- the connection -------------------------------------------------------------

  void _onMediaState(MediaState state) {
    if (_closed) return;
    final first = state == MediaState.connected && !(_call?.mediaEverConnected ?? true);
    peer().setMediaState(id, state);
    if (first) unawaited(_checkPath());
    changed();
  }

  /// While connected, the pair in use must be the tailnet's at both ends:
  /// libwebrtc binds every interface, and may nominate a pair from one whose
  /// candidate was never sent (ticket 02). On one machine it first takes a
  /// peer-reflexive LAN address to the tailnet's and moves to the tailnet's
  /// own pair within half a second, so the pair is read once ICE has
  /// settled, [pathSettle] after connecting, then every [pathEvery]; one
  /// outside the tailnet ends the call as an error.
  Future<void> _checkPath() async {
    var logged = false;
    for (var wait = pathSettle; ; wait = pathEvery) {
      await Future<void>.delayed(wait);
      final session = _session;
      final call = _call;
      if (session == null || !_live || call == null) return;
      if (call.media != MediaState.connected) continue;
      final path = await session.selectedPath();
      if (path == null) continue;
      final local = ipOf(path.local)?.address, remote = ipOf(path.remote)?.address;
      if (local != null && remote != null && inTailnet(local) && inTailnet(remote)) {
        if (!logged) log?.call('${callLabel(id)}: media over $path');
        logged = true;
        continue;
      }
      log?.call('${callLabel(id)}: media outside the tailnet, $path: ending it');
      if (call.live) peer().endCall(id, reason: 'error');
      return;
    }
  }

  static const pathSettle = Duration(seconds: 2);
  static const pathEvery = Duration(seconds: 5);

  /// Hang up the media: the microphone, the camera and the connection.
  Future<void> close() async {
    if (_closed) return;
    _closed = true;
    final session = _session;
    _session = null;
    _waiting.clear();
    cameraOn = false;
    if (session != null) {
      final video = await session.videoStats().catchError((Object _) => null);
      if (video != null) log?.call('${callLabel(id)}: video $video');
      await session.close();
    }
    children.cameraClosed();
  }
}
