/// Voice messages and calls in the app: recording and playing, the call in
/// progress, its ring, its notifications, mute and volume. The protocol,
/// the codec and a call's frames are the core's (ChatPeer, VoiceRecorder,
/// CallMedia); the devices are [AudioDevices]'. A call between two
/// companion apps runs on WebRTC instead, its media a [WebrtcCall]'s.
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
import 'webrtc_call.dart';

class Talk {
  Talk({
    required this.peer,
    required this.audio,
    this.engine,
    this.route,
    required this.notifier,
    required this.foreground,
    required this.away,
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

  /// A WebRTC call's loudspeaker or earpiece, the screen, and the camera
  /// while the app is away; Android's.
  final CallRoute? route;
  final Notifier notifier;
  final bool Function() foreground;

  /// Whether the app is out of sight (ChatModel.away).
  final bool Function() away;

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

  /// Whether `p` is allowed, asked the first time it is needed: the
  /// microphone for a recording or a call, the camera when first switched
  /// on in a call.
  Future<bool> _allowed(AppPermission p) async => permissions == null || await permissions!.need(p);

  /// Start recording a voice message for `box`; why not, or null.
  Future<String?> startRecording(String box) async {
    if (recordingBox != null) return 'already recording';
    if (_callLive) return 'not during a call';
    if (!await _allowed(AppPermission.microphone)) return 'The microphone is not allowed: voice messages need it.';
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
  String? _echoCancellation;

  /// The call's WebRTC media, on a call between companion apps.
  WebrtcCall? _webrtc;

  /// What removes the speaker from the microphone, for the call screen.
  String? get echoCancellation => _webrtc?.echoCancellation ?? _echoCancellation;

  /// Why the call has no audio, if it has none.
  String? get audioProblem => _audioProblem ?? _webrtc?.audioProblem;
  String? _audioProblem;
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
    _webrtc?.muted = on;
    changed();
  }

  /// 0 to 2; 1 plays the other side as it comes.
  double get volume => _volume;
  set volume(double v) {
    _volume = v.clamp(0, 2);
    _media?.volume = _volume;
    _webrtc?.volume = _volume;
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
    if (!await _allowed(AppPermission.microphone)) {
      _audioProblem = 'The microphone is not allowed: calls need it.';
      changed();
      return null;
    }
    await stopPlaying();
    _muted = false;
    _audioProblem = null;
    final id = await peer().call(callee(box), video: video);
    callId = id;
    _webrtcFor(id, camera: video);
    changed();
    return id;
  }

  /// Answer the ringing call (or the one named), with our camera on if
  /// `video`: it is never on unless asked.
  Future<void> answer({String? id, bool video = false}) async {
    id ??= callId;
    final c = id == null ? null : peer().calls[id];
    if (c == null || c.state != 'ringing') return;
    callId = id;
    _audioProblem = null;
    if (!await _allowed(AppPermission.microphone)) {
      _audioProblem = 'The microphone is not allowed: calls need it.';
    }
    if (recordingBox != null) await stopRecording(send: false);
    await stopPlaying();
    _muted = false;
    _webrtcFor(id!, camera: video);
    peer().answerCall(id);
  }

  /// The WebRTC media for a call that offers it, while we have an engine.
  void _webrtcFor(String id, {required bool camera}) {
    final engine = this.engine, c = peer().calls[id];
    if (engine == null || c == null || !c.live || !c.transports.contains(transportWebrtc)) return;
    if (_webrtc?.id == id) return;
    unawaited(_webrtc?.close());
    _webrtc =
        WebrtcCall(
            id: id,
            engine: engine,
            peer: peer,
            children: children,
            changed: changed,
            camera: camera,
            cameraAllowed: () => _allowed(AppPermission.camera),
            route: route,
            away: _cameraAway,
            log: log,
          )
          ..muted = _muted
          ..volume = _volume;
  }

  /// Whether our camera is let go: the app away, where the camera pauses.
  bool _cameraAway() => (route?.cameraPausesAway ?? false) && away();

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
    _webrtc?.followRoute(); // their camera may have come or gone
    if (c.state == 'ringing' && !c.mine) {
      _ringFor(id, c);
    } else {
      _stopRing(id);
    }
    if (c.state == 'connected' && c.webrtc) {
      _webrtcFor(id, camera: false); // answered by the core's own policy
      unawaited(_webrtc?.start());
    } else if (c.state == 'connected' && _media == null) {
      unawaited(_stopWebrtc());
      unawaited(_startMedia(id));
    }
    if (!c.live) {
      unawaited(_stopMedia());
      if (c.missed) unawaited(notifier.show(_missed(c), 'from ${nameOf(c.peer)}'));
      log?.call('${callLabel(id)} ${c.state}${c.transport == null ? '' : ', over ${c.transport}'}');
    }
    changed();
  }

  static String _missed(Call c) => c.video ? 'Missed video call' : 'Missed call';

  Future<void> _startMedia(String id) async {
    final CallDevices devices;
    try {
      devices = await audio.call();
    } on AudioUnavailable catch (e) {
      _audioProblem = '$e';
      changed();
      return;
    }
    if (callId != id || !(peer().calls[id]?.live ?? false)) {
      await devices.speaker.close();
      await devices.close();
      return;
    }
    _devices = devices;
    _echoCancellation = devices.echoCancellation;
    try {
      _media = CallMedia(peer(), id, mic: devices.microphone, speaker: devices.speaker)
        ..muted = _muted
        ..volume = _volume
        ..start();
    } on OpusUnavailable catch (e) {
      _audioProblem = '$e';
      await devices.speaker.close();
      await devices.close();
      _devices = null;
    }
    changed();
  }

  Future<void> _stopMedia() async {
    final media = _media, devices = _devices;
    _media = null;
    _devices = null;
    await media?.stop();
    await devices?.close();
    await _stopWebrtc();
  }

  Future<void> _stopWebrtc() async {
    final webrtc = _webrtc;
    _webrtc = null;
    _audioProblem ??= webrtc?.audioProblem; // kept for the ended call's screen
    await webrtc?.close();
  }

  /// The other side's SDP or candidate (the core's callRtc).
  void onRtc(CallRtc r) {
    if (r.call == _webrtc?.id) _webrtc!.take(r.payload);
  }

  // -- the camera, on a WebRTC call: WebrtcCall's -------------------------------

  /// This machine's cameras, once the call's media has started; empty
  /// with none.
  List<Camera>? get cameras => _webrtc?.cameras;

  /// Whether our camera is sending.
  bool get cameraOn => _webrtc?.cameraOn ?? false;

  /// Why the camera is off when it was asked on.
  String? get cameraProblem => _webrtc?.cameraProblem;

  /// Whether the call screen offers the camera switch: a WebRTC call.
  bool get cameraSwitch {
    final c = call;
    return c != null && c.live && (c.webrtc || (c.state == 'offered' && c.transports.contains(transportWebrtc)));
  }

  bool get canSwitchCamera => _webrtc?.canSwitchCamera ?? false;

  /// Whether the camera switch shows on: sending, or asked for before the
  /// call's media has started (Video call, while it rings).
  bool get cameraSwitchOn => _webrtc?.cameraSwitchOn ?? false;

  /// Whether the other side's video is to be drawn.
  bool get remoteVideoShown => _webrtc?.remoteVideoShown ?? false;

  Widget? remoteVideo() => _webrtc?.remoteVideo();
  Widget? localVideo() => _webrtc?.localVideo();

  /// The video's frames so far, on a WebRTC call.
  Future<VideoStats?> videoStats() async => _webrtc?.videoStats();

  /// Our camera on or off: before the call's media starts, as it starts.
  Future<void> setCameraOn(bool on) async => _webrtc?.setCameraOn(on);

  /// The app went out of sight, or came back ([away]): on Android our
  /// camera pauses meanwhile, the call going on in the listening service.
  Future<void> awayChanged() async => _webrtc?.followAway();

  /// The next camera, round and back to the first.
  Future<void> switchCamera() async => _webrtc?.switchCamera();

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
