import 'dart:async';
import 'dart:math';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:thistle_chat/src/chat_model.dart';
import 'package:thistle_chat/src/audio/devices.dart';
import 'package:thistle_chat/src/background.dart';
import 'package:thistle_chat/src/notifier.dart';
import 'package:thistle_chat/src/permissions.dart';
import 'package:thistle_chat/src/tailnet.dart';
import 'package:thistle_chat/src/ui/call_page.dart';

class TestTailnet {
  TestTailnet(this.status);
  TailscaleStatus status;
  Future<TailscaleStatus> read() async => status;
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

  /// Whether each ring was asked with the app in front.
  final List<bool> rangInFront = [];
  final List<String> stoppedRinging = [];

  /// Android's notifier rings the phone itself.
  @override
  final bool ringsItself;
  @override
  Future<void> show(String title, String body, {String? conversation}) async => shown.add(Shown(title, body));
  @override
  Future<void> ringing(String caller, String callId, {bool inFront = false}) async {
    rang.add(caller);
    rangInFront.add(inFront);
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

  @override
  Future<CallDevices> call() async =>
      CallDevices(pacedAudio(_tone), _Kept(null, into: callOut), 'none (test)', () async => callClosed = true);
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

/// A model listening on loopback, with the tailnet `tailnet` says.
ChatModel testModel(
  TestTailnet tailnet,
  Notifier notifier, {
  int discoveryPort = defaultPort,
  BackgroundService? service,
  FakePermissions? permissions,
  String store = ':memory:',
  AudioDevices? audio,
  Future<ProbeResult?> Function(PeerAddress address)? probe,
}) => ChatModel(
  probe: probe,
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
