// Two of the app's models in one process, calling each other over WebRTC
// through flutter_webrtc and the real libwebrtc, on this machine's tailnet
// address: what the fake engine of test/ stands in for. It needs Tailscale
// up, a sound server and a display, so CI does not run it; it prints the
// calls' log and exits 0 if both calls connected over the tailnet:
//
//     cd companion/app
//     flutter build linux --release -t tool/webrtc_call_check.dart \
//         --dart-define=TAILNET_IP=$(tailscale ip -4)
//     xvfb-run -a build/linux/x64/release/bundle/thistle_chat
//
// The bundle is this check, not the app, until the app is built again.
//
// Both ends are this machine, so the path is the tailnet address to
// itself, not WireGuard between two machines, and nobody listens: it shows
// signalling and the connection, not sound. The by-hand run is in
// .scratch/tailnet-video/issues/03-*.

// ignore_for_file: avoid_print

import 'dart:async';
import 'dart:io';

import 'package:flutter/widgets.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:thistle_chat/src/chat_model.dart';
import 'package:thistle_chat/src/tailnet.dart';
import 'package:thistle_chat/src/webrtc_engine.dart';

import '../test/helpers.dart';

const tailnetIp = String.fromEnvironment('TAILNET_IP');

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(
    const Directionality(
      textDirection: TextDirection.ltr,
      child: Center(child: Text('WebRTC call check')),
    ),
  );
  var ok = false;
  try {
    ok = await check();
  } on Object catch (e, st) {
    print('FAILED: $e\n$st');
  }
  print(ok ? 'PASS' : 'FAIL');
  exit(ok ? 0 : 1);
}

Future<bool> check() async {
  if (tailnetIp.isEmpty) throw ArgumentError('--dart-define=TAILNET_IP=\$(tailscale ip -4)');
  final status = TestTailnet(
    parseTailscaleStatus(File('test/fixtures/tailscale_status_running.json').readAsStringSync()),
  );
  final lines = <String>[];
  ChatModel model(String name) => ChatModel(
    identity: Identity.generate(),
    openStore: () => Store.open(':memory:'),
    name: name,
    readStatus: status.read,
    notifier: RecordingNotifier(),
    listenHost: tailnetIp,
    port: 0,
    refreshEvery: Duration.zero,
    audio: FakeAudio(),
    callEngine: WebrtcCallEngine(log: (l) => lines.add('[$name] $l')),
    log: lines.add,
  );
  final a = model('desk-a'), b = model('desk-b');
  await a.start();
  await b.start();
  b.peer.addContact(a.listening!, name: 'a');
  b.peer.sendText('a', 'hi');
  await eventually(() => a.requests.isNotEmpty);
  a.accept(b.identity.keyHex);
  await eventually(() => b.peer.contact('a')!.acceptedByThem == true);

  var ok = true;
  for (var round = 1; round <= 2; round++) {
    final clock = Stopwatch()..start();
    final id = (await a.talk.startCall(b.identity.keyHex))!;
    await eventually(() => b.talk.call?.state == 'ringing');
    await b.talk.answer();
    await eventually(
      () => a.peer.calls[id]!.media == MediaState.connected && b.peer.calls[id]!.media == MediaState.connected,
      timeout: const Duration(seconds: 15),
    );
    print('call $round: media connected ${clock.elapsedMilliseconds} ms after the offer');
    await Future<void>.delayed(const Duration(seconds: 4)); // past the pair check
    for (final (who, m) in [('a', a), ('b', b)]) {
      final c = m.peer.calls[id]!;
      final good = c.transport == 'webrtc' && c.state == 'connected' && c.remoteCamera == CameraState.off;
      print(
        'call $round, $who: ${c.transport} ${c.state}, their camera ${c.remoteCamera}, '
        'echo cancellation ${m.talk.echoCancellation}${good ? '' : '  <- wrong'}',
      );
      ok &= good;
    }
    a.talk.hangUp();
    await eventually(() => b.peer.calls[id]!.state == 'ended:hangup');
    a.talk.dismissCall();
    b.talk.dismissCall();
    await Future<void>.delayed(const Duration(seconds: 1));
  }
  ok &= await videoCall(a, b);
  await a.close();
  await b.close();
  print(lines.where((l) => l.contains('call ') || l.contains('camera')).join('\n'));
  final sent = lines.where((l) => l.contains('candidate sent'));
  ok &= sent.isNotEmpty && sent.every((l) => l.contains('tailnet'));
  ok &= lines.where((l) => l.contains('media over host/udp')).length == 6;
  return ok;
}

/// A video call: `a` calls with its camera on and `b` answers without; then
/// `a` turns its camera off and `b` turns its own on. One process has one
/// camera, so the two never send at once. Each side's frames come from
/// getStats, and must be at most 1280x720.
Future<bool> videoCall(ChatModel a, ChatModel b) async {
  var ok = true;
  final id = (await a.talk.startCall(b.identity.keyHex, video: true))!;
  await eventually(() => b.talk.call?.state == 'ringing');
  print('call 3: offered with video ${b.peer.calls[id]!.video}');
  await b.talk.answer();
  await eventually(() => b.peer.calls[id]!.remoteCamera == CameraState.on, timeout: const Duration(seconds: 15));
  print('call 3: cameras at a ${a.talk.cameras}, Switch camera ${a.talk.canSwitchCamera ? 'shown' : 'not shown'}');

  Future<bool> flows(ChatModel from, ChatModel to, String what, {int seconds = 4}) async {
    await Future<void>.delayed(Duration(seconds: seconds));
    final sent = await from.talk.videoStats(), got = await to.talk.videoStats();
    print('call 3, $what: sender $sent; receiver $got');
    final size = sent?.sent?.split('x').map(int.parse).toList();
    final good =
        sent != null &&
        got != null &&
        sent.framesSent > 30 &&
        got.framesReceived > 30 &&
        size != null &&
        size[0] <= 1280 &&
        size[1] <= 720;
    if (!good) print('  <- wrong');
    return good;
  }

  ok &= a.talk.cameraOn && !b.talk.cameraOn;
  ok &= await flows(a, b, "a's camera");
  ok &= await flows(a, b, "a's camera, 12 s on", seconds: 8); // the bitrate climbs
  if (a.talk.canSwitchCamera) {
    await a.talk.switchCamera();
    ok &= await flows(a, b, "a's second camera");
  }
  await a.talk.setCameraOn(false);
  await eventually(() => b.peer.calls[id]!.remoteCamera == CameraState.off);
  final before = (await b.talk.videoStats())?.framesReceived ?? 0;
  await b.talk.setCameraOn(true);
  await eventually(() => a.peer.calls[id]!.remoteCamera == CameraState.on);
  print('call 3: b camera on (${b.talk.cameraProblem ?? 'no problem'}); b had received $before frames');
  ok &= b.talk.cameraOn;
  ok &= await flows(b, a, "b's camera");
  a.talk.hangUp();
  await eventually(() => b.peer.calls[id]!.state == 'ended:hangup');
  a.talk.dismissCall();
  b.talk.dismissCall();
  await Future<void>.delayed(const Duration(seconds: 1));
  return ok;
}
