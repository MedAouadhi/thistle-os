/// Voice messages and calls in the app: recording and playing, the call in
/// progress, its ring, its notifications, mute and volume. The protocol,
/// the codec and a call's frames are the core's (ChatPeer, VoiceRecorder,
/// CallMedia); the devices are [AudioDevices]'. A call between two
/// companion apps runs on WebRTC instead, its media the [CallEngine]'s.
library;

import 'dart:async';
import 'dart:math';
import 'dart:typed_data';

import 'package:flutter/widgets.dart' show Widget;
import 'package:tailnet_chat_core/tailnet_chat_core.dart';

import 'audio/devices.dart';
import 'call_engine.dart';
import 'child_processes.dart';
import 'notifier.dart';
import 'permissions.dart';

class Talk {
  Talk({
    required this.peer,
    required this.audio,
    this.engine,
    required this.notifier,
    required this.foreground,
    required this.changed,
    required this.sendVoice,
    required this.callee,
    this.permissions,
    ChildProcesses? children,
    this.log,
  }) : children = children ?? ChildProcesses();

  final ChatPeer Function() peer;
  final AudioDevices audio;

  /// WebRTC calls' media; null where the app announces no `call:webrtc`.
  final CallEngine? engine;
  final Notifier notifier;
  final bool Function() foreground;

  /// Something on screen may have changed.
  final void Function() changed;

  /// Send a recorded voice message to a conversation.
  final void Function(String box, Uint8List oggOpus, int ms) sendVoice;

  /// How to name a conversation's contact to the core, to call it.
  final Object Function(String box) callee;
  final Permissions? permissions;

  /// None starts while our camera is on.
  final ChildProcesses children;
  final void Function(String)? log;

  // -- recording ----------------------------------------------------------------

  String? recordingBox;
  VoiceRecorder? _recorder;
  StreamSubscription<Int16List>? _mic;
  Timer? _recordTick;

  int get recordingMs => _recorder?.ms ?? 0;

  Future<bool> _microphone() async => permissions == null || await permissions!.need(AppPermission.microphone);

  /// Start recording a voice message for `box`; why not, or null.
  Future<String?> startRecording(String box) async {
    if (recordingBox != null) return 'already recording';
    if (_callLive) return 'not during a call';
    if (!await _microphone()) return 'The microphone is not allowed: voice messages need it.';
    final VoiceRecorder rec;
    try {
      rec = VoiceRecorder();
    } on OpusUnavailable catch (e) {
      return '$e';
    }
    try {
      final mic = await audio.recorder();
      _recorder = rec;
      recordingBox = box;
      _mic = mic.listen((pcm) {
        rec.add(pcm);
        if (rec.full) unawaited(stopRecording()); // two minutes: it goes as it is
      });
    } on AudioUnavailable catch (e) {
      rec.cancel();
      return '$e';
    }
    _recordTick = Timer.periodic(const Duration(milliseconds: 250), (_) => changed());
    changed();
    return null;
  }

  /// Stop recording; send what was recorded unless `send` is false.
  Future<void> stopRecording({bool send = true}) async {
    final rec = _recorder, box = recordingBox;
    if (rec == null || box == null) return;
    _recorder = null;
    recordingBox = null;
    _recordTick?.cancel();
    await _mic?.cancel();
    _mic = null;
    if (send && rec.ms > 0) {
      final m = rec.finish();
      sendVoice(box, m.bytes, max(1, m.ms));
    } else {
      rec.cancel();
    }
    changed();
  }

  // -- playing ------------------------------------------------------------------

  String? playingId;
  int playedMs = 0;
  Timer? _player;
  AudioOut? _out;

  /// Play a voice message from its start; another playing stops.
  Future<String?> play(Message m) async {
    await stopPlaying();
    if (m.voice == null) return 'not a voice message';
    final Int16List pcm;
    try {
      pcm = decodeVoice(m.voice!);
    } on Object catch (e) {
      return 'cannot play it: $e';
    }
    final AudioOut out;
    try {
      out = await audio.player();
    } on AudioUnavailable catch (e) {
      return '$e';
    }
    _out = out;
    playingId = m.id;
    playedMs = 0;
    final clock = Stopwatch()..start();
    var at = 0;
    void feed() {
      // Kept 60 ms ahead of the clock, so Stop stops it at once.
      final due = ((clock.elapsedMilliseconds + 60) * sampleRate ~/ 1000).clamp(0, pcm.length);
      if (due > at) {
        out.write(Int16List.sublistView(pcm, at, due));
        at = due;
      }
      playedMs = min(clock.elapsedMilliseconds, pcm.length * 1000 ~/ sampleRate);
      if (at >= pcm.length && clock.elapsedMilliseconds >= pcm.length * 1000 ~/ sampleRate) {
        unawaited(stopPlaying());
      }
      changed();
    }

    feed();
    _player = Timer.periodic(const Duration(milliseconds: 20), (_) => feed());
    changed();
    return null;
  }

  Future<void> stopPlaying() async {
    _player?.cancel();
    _player = null;
    final out = _out;
    _out = null;
    if (playingId == null) return;
    playingId = null;
    changed();
    await out?.close();
  }

  // -- calls --------------------------------------------------------------------

  /// The call on screen: live, or just ended (until [dismissCall]).
  String? callId;
  CallMedia? _media;
  CallDevices? _devices;
  String? echoCancellation;

  /// A WebRTC call's media, once started; the other side's SDP and
  /// candidates wait in `_rtcWaiting` until then, in order.
  CallSession? _session;
  bool _sessionStarting = false;
  final List<CallRtc> _rtcWaiting = [];
  Future<void> _rtcTaken = Future.value();

  /// Why the call has no audio, if it has none.
  String? audioProblem;
  bool _muted = false;
  double _volume = 1;
  AudioOut? _ring;
  Timer? _ringTimer;

  /// The call ringing now, if one is.
  String? _ringing;

  Call? get call => callId == null ? null : peer().calls[callId];
  bool get _callLive => call?.live ?? false;

  /// A call coming in, not one we placed.
  bool get incoming => call != null && !call!.mine;

  bool get muted => _muted;
  set muted(bool on) {
    _muted = on;
    _media?.muted = on;
    _session?.muted = on;
    changed();
  }

  /// 0 to 2; 1 plays the other side as it comes.
  double get volume => _volume;
  set volume(double v) {
    _volume = v.clamp(0, 2);
    _media?.volume = _volume;
    _session?.volume = _volume;
    changed();
  }

  /// How long the call has been connected.
  Duration get connectedFor {
    final c = call;
    if (c == null || c.state != 'connected') return Duration.zero;
    return DateTime.now().difference(c.since);
  }

  String nameOf(String key) => peer().contact(key)?.name ?? 'a contact';

  /// Call a contact, with our camera on from the first ring if `video`; the
  /// call's id, or null if one is already up. A contact who cannot be
  /// reached fails at once (`ended:unreachable`).
  Future<String?> startCall(String box, {bool video = false}) async {
    if (_callLive) return null;
    if (recordingBox != null) await stopRecording(send: false);
    if (!await _microphone()) {
      audioProblem = 'The microphone is not allowed: calls need it.';
      changed();
      return null;
    }
    await stopPlaying();
    _muted = false;
    audioProblem = null;
    _wantCamera = video;
    cameraProblem = null;
    final id = await peer().call(callee(box), video: video);
    callId = id;
    changed();
    return id;
  }

  /// Answer the ringing call (or the one named), with our camera on if
  /// `video`: it is never on unless asked.
  Future<void> answer([String? id, bool video = false]) async {
    id ??= callId;
    final c = id == null ? null : peer().calls[id];
    if (c == null || c.state != 'ringing') return;
    callId = id;
    if (!await _microphone()) {
      audioProblem = 'The microphone is not allowed: calls need it.';
    }
    if (recordingBox != null) await stopRecording(send: false);
    await stopPlaying();
    _muted = false;
    _wantCamera = video;
    cameraProblem = null;
    peer().answerCall(id!);
  }

  void decline([String? id]) {
    id ??= callId;
    final c = id == null ? null : peer().calls[id];
    if (c == null || c.state != 'ringing') return;
    peer().answerCall(id!, decline: 'declined');
  }

  void hangUp() {
    final id = callId;
    if (id != null && _callLive) peer().endCall(id);
  }

  /// Take the ended call off the screen.
  void dismissCall() {
    if (_callLive) return;
    callId = null;
    changed();
  }

  /// A call changed state (the core's `call` event).
  void onCallEvent(ChatEvent e) {
    final id = e.id;
    if (id == null) return;
    final c = peer().calls[id];
    if (c == null) return;
    // A second call while one is up is the core's to answer busy; it is
    // only shown as missed.
    if (callId != null && callId != id && _callLive) {
      if (c.missed) unawaited(notifier.show(_missed(c), 'from ${nameOf(c.peer)} while you were in a call'));
      return;
    }
    callId = id;
    if (c.state == 'ringing' && !c.mine) {
      _ringFor(id, c);
    } else {
      _stopRing(id);
    }
    if (c.state == 'connected' && c.webrtc) {
      if (_session == null && !_sessionStarting) unawaited(_startWebrtc(id, c));
    } else if (c.state == 'connected' && _media == null) {
      unawaited(_startMedia(id));
    }
    if (!c.live) {
      unawaited(_stopMedia());
      if (c.missed) unawaited(notifier.show(_missed(c), 'from ${nameOf(c.peer)}'));
      log?.call('call ${id.substring(0, 8)} ${c.state}${c.transport == null ? '' : ', over ${c.transport}'}');
    }
    changed();
  }

  static String _missed(Call c) => c.video ? 'Missed video call' : 'Missed call';

  Future<void> _startMedia(String id) async {
    final CallDevices devices;
    try {
      devices = await audio.call();
    } on AudioUnavailable catch (e) {
      audioProblem = '$e';
      changed();
      return;
    }
    if (callId != id || !(peer().calls[id]?.live ?? false)) {
      await devices.speaker.close();
      await devices.close();
      return;
    }
    _devices = devices;
    echoCancellation = devices.echoCancellation;
    try {
      _media = CallMedia(peer(), id, mic: devices.microphone, speaker: devices.speaker)
        ..muted = _muted
        ..volume = _volume
        ..start();
    } on OpusUnavailable catch (e) {
      audioProblem = '$e';
      await devices.speaker.close();
      await devices.close();
      _devices = null;
    }
    changed();
  }

  Future<void> _stopMedia() async {
    final media = _media, devices = _devices, session = _session, id = callId;
    _media = null;
    _devices = null;
    _session = null;
    _rtcWaiting.clear();
    _wantCamera = false;
    cameraOn = false;
    cameras = null;
    _cameraAt = 0;
    await media?.stop();
    await devices?.close();
    if (session != null) {
      final video = await session.videoStats().catchError((Object _) => null);
      if (video != null && id != null) log?.call('call ${id.substring(0, 8)}: video $video');
    }
    await session?.close();
    children.cameraClosed();
  }

  // -- WebRTC calls: the media is the engine's; its SDP and candidates go
  // through the core as call_rtc, which keeps the tailnet's alone.

  /// The other side's SDP or candidate (the core's callRtc).
  void onRtc(CallRtc r) {
    if (r.call != callId) return;
    final session = _session;
    if (session == null) {
      _rtcWaiting.add(r);
    } else {
      _take(session, r);
    }
  }

  /// One at a time, in the order they came: a candidate never reaches the
  /// connection before the SDP it belongs to.
  void _take(CallSession session, CallRtc r) =>
      _rtcTaken = _rtcTaken.then((_) => session.take(sdp: r.sdp, candidate: r.candidate)).catchError((Object e) {
        log?.call('call ${r.call.substring(0, 8)}: WebRTC refused what the other side sent: $e');
      });

  Future<void> _startWebrtc(String id, Call c) async {
    final engine = this.engine;
    if (engine == null) return;
    _sessionStarting = true;
    log?.call('call ${id.substring(0, 8)} over webrtc');
    final CallSession session;
    try {
      session = await engine.start(
        offerer: c.mine,
        send: ({sdp, candidate}) => peer().sendRtc(id, sdp: sdp, candidate: candidate),
        onState: (state) => _onMediaState(id, state),
      );
    } on Object catch (e) {
      _sessionStarting = false;
      audioProblem = 'The call has no audio: $e';
      log?.call('call ${id.substring(0, 8)}: WebRTC did not start: $e');
      if (peer().calls[id]?.live ?? false) peer().endCall(id, reason: 'error');
      changed();
      return;
    }
    _sessionStarting = false;
    if (callId != id || !(peer().calls[id]?.live ?? false)) {
      await session.close();
      return;
    }
    _session = session
      ..muted = _muted
      ..volume = _volume;
    echoCancellation = session.echoCancellation;
    for (final r in List.of(_rtcWaiting)) {
      _take(session, r);
    }
    _rtcWaiting.clear();
    List<String> found;
    try {
      found = await engine.cameras();
    } on Object catch (e) {
      log?.call('call ${id.substring(0, 8)}: no camera found: $e');
      found = const [];
    }
    if (_session != session) return;
    cameras = found;
    // Said at the answer, as every change will be.
    if (found.isEmpty) {
      peer().setCamera(id, 'none');
    } else if (_wantCamera) {
      await _applyCamera(session, id, on: true);
    } else {
      peer().setCamera(id, 'off');
    }
    changed();
  }

  // -- the camera, on a WebRTC call ---------------------------------------------

  /// What our camera should be until the call's media starts.
  bool _wantCamera = false;

  /// This machine's cameras, once the call's media has started; empty
  /// with none.
  List<String>? cameras;
  int _cameraAt = 0;

  /// Whether our camera is sending.
  bool cameraOn = false;

  /// Why the camera is off when it was asked on.
  String? cameraProblem;

  /// Whether the call screen offers the camera switch: a WebRTC call.
  bool get cameraSwitch {
    final c = call;
    return c != null && c.live && (c.webrtc || (c.state == 'offered' && c.transports.contains('webrtc')));
  }

  bool get canSwitchCamera => (cameras?.length ?? 0) > 1;

  /// Whether the camera switch shows on: sending, or asked for before the
  /// call's media has started (Video call, while it rings).
  bool get cameraSwitchOn => _session == null ? _wantCamera : cameraOn;

  /// Whether the other side's video is to be drawn.
  bool get remoteVideoShown => _session != null && call?.remoteCamera == 'on';

  Widget? remoteVideo() => _session?.remoteVideo();
  Widget? localVideo() => cameraOn ? _session?.localVideo() : null;

  /// The video's frames so far, on a WebRTC call.
  Future<VideoStats?> videoStats() async => _session?.videoStats();

  /// Our camera on or off: before the call's media starts, as it starts.
  Future<void> setCameraOn(bool on) async {
    final session = _session, id = callId;
    cameraProblem = null;
    if (session == null || id == null) {
      _wantCamera = on;
      changed();
      return;
    }
    if (on == cameraOn || (on && (cameras ?? const []).isEmpty)) return;
    await _applyCamera(session, id, on: on);
    changed();
  }

  /// The next camera, round and back to the first.
  Future<void> switchCamera() async {
    final session = _session, id = callId, list = cameras;
    if (session == null || id == null || list == null || list.length < 2 || !cameraOn) return;
    _cameraAt = (_cameraAt + 1) % list.length;
    await _applyCamera(session, id, on: true);
    changed();
  }

  /// No child process starts while the camera is on, and the camera waits
  /// for those running (child_processes.dart); a camera that gives no
  /// picture is turned off and said so.
  Future<void> _applyCamera(CallSession session, String id, {required bool on}) async {
    final camera = on ? cameras![_cameraAt] : null;
    if (on) await children.cameraOpening();
    try {
      await session.setCamera(camera);
      cameraOn = on;
    } on CameraUnavailable catch (e) {
      cameraOn = false;
      cameraProblem = 'The camera is off: $e. Another program may be using it.';
      log?.call('call ${id.substring(0, 8)}: camera $camera gave no picture: $e');
    }
    if (!cameraOn) children.cameraClosed();
    if (_session != session) return;
    log?.call('call ${id.substring(0, 8)}: our camera ${cameraOn ? 'on, $camera' : 'off'}');
    peer().setCamera(id, cameraOn ? 'on' : 'off');
  }

  void _onMediaState(String id, String state) {
    final first = state == 'connected' && !(peer().calls[id]?.mediaEverConnected ?? true);
    peer().setMediaState(id, state);
    if (first) unawaited(_checkPath(id));
    changed();
  }

  /// While connected, the pair in use must be the tailnet's at both ends:
  /// libwebrtc binds every interface, and may nominate a pair from one whose
  /// candidate was never sent (ticket 02). On one machine it first takes a
  /// peer-reflexive LAN address to the tailnet's and moves to the tailnet's
  /// own pair within half a second, so the pair is read once ICE has
  /// settled, [pathSettle] after connecting, then every [pathEvery]; one
  /// outside the tailnet ends the call as an error.
  Future<void> _checkPath(String id) async {
    var logged = false;
    for (var wait = pathSettle; ; wait = pathEvery) {
      await Future<void>.delayed(wait);
      final session = _session;
      final call = peer().calls[id];
      if (session == null || callId != id || call == null || !call.live) return;
      if (call.media != 'connected') continue;
      final path = await session.selectedPath();
      if (path == null) continue;
      final local = ipOf(path.local)?.address, remote = ipOf(path.remote)?.address;
      if (local != null && remote != null && inTailnet(local) && inTailnet(remote)) {
        if (!logged) log?.call('call ${id.substring(0, 8)}: media over $path');
        logged = true;
        continue;
      }
      log?.call('call ${id.substring(0, 8)}: media outside the tailnet, $path: ending it');
      if (call.live) peer().endCall(id, reason: 'error');
      return;
    }
  }

  static const pathSettle = Duration(seconds: 2);
  static const pathEvery = Duration(seconds: 5);

  /// Ring for a call coming in, once: the notifier with the app out of
  /// sight, and the phone's ringtone or else the app's own ring.
  void _ringFor(String id, Call c) {
    if (_ringing == id) return;
    _ringing = id;
    if (!notifier.ringsItself) _startRing();
    unawaited(notifier.ringing(nameOf(c.peer), id, inFront: foreground(), video: c.video));
    unawaited(permissions?.checkFillScreen());
  }

  /// A two-tone ring: 0.4 s on, 0.2 s off, 0.4 s on, 2 s off.
  void _startRing() {
    if (_ringTimer != null) return;
    _ringTimer = Timer(Duration.zero, () {}); // marks it started
    unawaited(() async {
      try {
        _ring = await audio.player(ring: true);
      } on AudioUnavailable {
        return;
      }
      if (_ringTimer == null) {
        await _ring?.close();
        _ring = null;
        return;
      }
      final clock = Stopwatch()..start();
      var sent = 0;
      _ringTimer = Timer.periodic(const Duration(milliseconds: 20), (_) {
        final due = clock.elapsedMilliseconds ~/ 20 + 3;
        while (sent < due) {
          _ring?.write(_ringFrame(sent++));
        }
      });
    }());
  }

  void _stopRing(String id) {
    if (_ringing != id) return;
    _ringing = null;
    _ringTimer?.cancel();
    _ringTimer = null;
    final r = _ring;
    _ring = null;
    unawaited(r?.close());
    unawaited(notifier.stopRinging(id));
  }

  static Int16List _ringFrame(int n) {
    final t = n % 150; // 3 s
    final on = t < 20 || (t >= 30 && t < 50);
    final out = Int16List(samplesPerFrame);
    if (!on) return out;
    for (var i = 0; i < samplesPerFrame; i++) {
      final s = (n * samplesPerFrame + i) / sampleRate;
      out[i] = (3000 * (sin(2 * pi * 440 * s) + sin(2 * pi * 480 * s))).round();
    }
    return out;
  }

  Future<void> close() async {
    await stopRecording(send: false);
    await stopPlaying();
    if (callId != null) _stopRing(callId!);
    hangUp();
    await _stopMedia();
  }
}
