import 'dart:async';
import 'dart:math';
import 'dart:typed_data';

import 'package:flutter/widgets.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:thistle_chat/src/chat_model.dart';
import 'package:thistle_chat/src/audio/devices.dart';
import 'package:thistle_chat/src/background.dart';
import 'package:thistle_chat/src/call_engine.dart';
import 'package:thistle_chat/src/notifier.dart';
import 'package:thistle_chat/src/permissions.dart';
import 'package:thistle_chat/src/tailnet.dart';
import 'package:thistle_chat/src/ui/call_page.dart';

class TestTailnet {
  TestTailnet(this.status);
  TailscaleStatus status;

  /// How many times it was read: `tailscale status` is a child process.
  int reads = 0;
  Future<TailscaleStatus> read() async {
    reads++;
    return status;
  }
}

class Shown {
  Shown(this.title, this.body);
  final String title;
  final String body;
  @override
  String toString() => '$title: $body';
}

class RecordingNotifier implements Notifier {
  RecordingNotifier({this.ringsItself = false});
  final List<Shown> shown = [];
  final List<String> rang = [];

  /// Whether each ring was asked with the app in front, and with video.
  final List<bool> rangInFront = [];
  final List<bool> rangVideo = [];
  final List<String> stoppedRinging = [];

  /// Android's notifier rings the phone itself.
  @override
  final bool ringsItself;
  @override
  Future<void> show(String title, String body, {String? conversation}) async => shown.add(Shown(title, body));
  @override
  Future<void> ringing(String caller, String callId, {bool inFront = false, bool video = false}) async {
    rang.add(caller);
    rangInFront.add(inFront);
    rangVideo.add(video);
  }

  @override
  Future<void> stopRinging(String callId) async => stoppedRinging.add(callId);
}

Int16List _tone(int i) => Int16List.fromList([
  for (var k = 0; k < samplesPerFrame; k++) (8000 * sin(2 * pi * 440 * (i * samplesPerFrame + k) / sampleRate)).round(),
]);

/// Audio devices for tests: a microphone that hears a 440 Hz tone, and
/// speakers that keep what they play.
class FakeAudio implements AudioDevices {
  final List<Int16List> played = [];
  final List<int> callOut = [];
  int rings = 0;
  bool callClosed = false;

  @override
  Future<Stream<Int16List>> recorder() async => pacedAudio(_tone);

  @override
  Future<AudioOut> player({bool ring = false}) async {
    if (ring) {
      rings++;
      return NoAudioOut();
    }
    return _Kept((pcm) => played.add(pcm));
  }

  /// How many times a call's devices were opened: a `ws-opus` call's, and
  /// on Linux the PipeWire echo canceller with them.
  int callsOpened = 0;

  @override
  Future<CallDevices> call() async {
    callsOpened++;
    return CallDevices(pacedAudio(_tone), _Kept(null, into: callOut), 'none (test)', () async => callClosed = true);
  }
}

class _Kept implements AudioOut {
  _Kept(this.onClose, {List<int>? into}) : _into = into ?? [];
  final void Function(Int16List)? onClose;
  final List<int> _into;
  @override
  void write(Int16List pcm) => _into.addAll(pcm);
  @override
  Future<void> close() async => onClose?.call(Int16List.fromList(_into));
}

/// Android's call window over the lock screen, shown and finished by hand.
class TestCallWindow extends ChangeNotifier implements CallWindow {
  @override
  bool shown = false;
  int finished = 0;

  void show() {
    shown = true;
    notifyListeners();
  }

  @override
  void finish() {
    finished++;
    shown = false;
    notifyListeners();
  }
}

class FakeService implements BackgroundService {
  bool running = false;
  final List<bool> calls = [];
  @override
  Future<void> setRunning(bool on) async {
    calls.add(on);
    running = on;
  }
}

/// The platform's permission prompts, answering `answer`.
class FakePermissions implements PermissionPlatform {
  PermissionAnswer answer = PermissionAnswer.granted;
  final Map<AppPermission, PermissionAnswer> state = {};
  final List<AppPermission> asked = [];
  int settingsOpened = 0;

  /// Whether calls may fill the screen (Android 14's full-screen intents).
  bool fillScreen = true;
  int fillScreenChecks = 0;
  int fillScreenOpened = 0;
  @override
  Future<bool> canFillScreen() async {
    fillScreenChecks++;
    return fillScreen;
  }

  @override
  Future<void> openFillScreenSettings() async => fillScreenOpened++;
  @override
  Future<PermissionAnswer> status(AppPermission p) async => state[p] ?? PermissionAnswer.denied;
  @override
  Future<PermissionAnswer> request(AppPermission p) async {
    asked.add(p);
    return state[p] = answer;
  }

  @override
  Future<void> openSettings() async => settingsOpened++;
}

/// A call media engine for tests: no WebRTC, but the signalling a real one
/// does. The offerer gives an SDP offer, the other an answer to it; each
/// gives its `candidates`; a session reports `connected` once it holds the
/// other's SDP and a candidate, unless `connects` is false. Its `cameras`
/// give a picture unless `picture` is false, and its video is a keyed box.
class FakeCallEngine implements CallEngine {
  FakeCallEngine({
    this.candidates = const [tailnetCandidate, lanCandidate],
    this.connects = true,
    this.path = const CallPath(local: '100.64.0.1', remote: '100.64.0.2', localType: 'host', remoteType: 'host'),
    this.cameraList = const ['Integrated Webcam'],
  });

  static const tailnetCandidate = 'candidate:1 1 udp 2122260223 100.64.0.1 47998 typ host generation 0';
  static const lanCandidate = 'candidate:3 1 udp 2122194687 192.168.2.131 40000 typ host generation 0';

  final List<String> candidates;
  bool connects;
  CallPath? path;
  List<String> cameraList;
  bool picture = true;
  final List<FakeCallSession> sessions = [];

  FakeCallSession get session => sessions.last;

  @override
  Future<List<String>> cameras() async => cameraList;

  @override
  Future<CallSession> start({
    required bool offerer,
    required SendRtc send,
    required void Function(String state) onState,
  }) async {
    final s = FakeCallSession(this, offerer, send, onState);
    sessions.add(s);
    if (offerer) send(sdp: {'type': 'offer', 'sdp': 'v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\n'});
    for (final c in candidates) {
      send(candidate: {'candidate': c, 'sdpMid': '0', 'sdpMLineIndex': 0});
    }
    return s;
  }
}

class FakeCallSession implements CallSession {
  FakeCallSession(this.engine, this.offerer, this._send, this._onState);
  final FakeCallEngine engine;
  final bool offerer;
  final SendRtc _send;
  final void Function(String) _onState;

  /// What it took from the other side, in order.
  final List<Map<String, Object?>> sdps = [];
  final List<String> candidates = [];
  bool muted = false;
  double volume = 1;
  bool closed = false;
  bool _connected = false;

  void state(String s) => _onState(s);

  @override
  String get echoCancellation => 'on (WebRTC, fake)';

  @override
  Future<void> take({Map<String, Object?>? sdp, Map<String, Object?>? candidate}) async {
    if (closed) return;
    if (sdp != null) {
      sdps.add(sdp);
      if (sdp['type'] == 'offer') _send(sdp: {'type': 'answer', 'sdp': 'v=0\r\no=- 3 4 IN IP4 127.0.0.1\r\n'});
    }
    if (candidate != null) candidates.add(candidate['candidate'] as String);
    if (!_connected && engine.connects && sdps.isNotEmpty && candidates.isNotEmpty) {
      _connected = true;
      _onState('connected');
    }
  }

  @override
  Future<CallPath?> selectedPath() async => engine.path;

  /// The camera sending now, null for none; and every one it was set to.
  String? camera;
  final List<String?> cameraLog = [];

  @override
  Future<void> setCamera(String? camera) async {
    cameraLog.add(camera);
    if (camera != null && !engine.picture) {
      this.camera = null;
      throw const CameraUnavailable('no picture came from it');
    }
    this.camera = camera;
  }

  @override
  Future<VideoStats?> videoStats() async => null;

  @override
  Widget remoteVideo() => const SizedBox.expand(key: Key('fake-remote-video'));

  @override
  Widget localVideo() => const SizedBox.expand(key: Key('fake-local-video'));

  @override
  Future<void> close() async {
    camera = null;
    closed = true;
  }
}

/// A model listening on loopback, with the tailnet `tailnet` says.
ChatModel testModel(
  TestTailnet tailnet,
  Notifier notifier, {
  int discoveryPort = defaultPort,
  BackgroundService? service,
  FakePermissions? permissions,
  String store = ':memory:',
  AudioDevices? audio,
  CallEngine? callEngine,
  Future<ProbeResult?> Function(PeerAddress address)? probe,
}) => ChatModel(
  probe: probe,
  callEngine: callEngine,
  audio: audio ?? FakeAudio(),
  identity: Identity.generate(),
  openStore: () => Store.open(store),
  background: service,
  permissions: permissions == null ? null : Permissions(permissions),
  name: 'laptop',
  readStatus: tailnet.read,
  notifier: notifier,
  listenHost: '127.0.0.1',
  port: 0,
  loopback: true,
  discoveryPort: discoveryPort,
  refreshEvery: Duration.zero,
);

/// Wait, in real time, until `condition` holds.
Future<void> eventually(bool Function() condition, {Duration timeout = const Duration(seconds: 5)}) async {
  final end = DateTime.now().add(timeout);
  while (!condition()) {
    if (DateTime.now().isAfter(end)) throw TimeoutException('condition not met', timeout);
    await Future<void>.delayed(const Duration(milliseconds: 20));
  }
}
