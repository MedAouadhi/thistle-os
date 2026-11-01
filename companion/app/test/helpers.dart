import 'dart:async';

import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:thistle_chat/src/chat_model.dart';
import 'package:thistle_chat/src/background.dart';
import 'package:thistle_chat/src/notifier.dart';
import 'package:thistle_chat/src/permissions.dart';
import 'package:thistle_chat/src/tailnet.dart';

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
  final List<Shown> shown = [];
  @override
  Future<void> show(String title, String body, {String? conversation}) async => shown.add(Shown(title, body));
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
}) => ChatModel(
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
