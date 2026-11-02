import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:thistle_chat/src/chat_model.dart';
import 'package:thistle_chat/src/permissions.dart';
import 'package:thistle_chat/src/tailnet.dart';
import 'package:thistle_chat/src/ui/app.dart';

import 'helpers.dart';

/// The screens, driven by taps and typing, against a real chat peer on
/// loopback: the device's side of the conversation.
void main() {
  late TestTailnet tailnet;
  late ChatModel app;
  late ChatPeer device;

  TailscaleStatus running() =>
      parseTailscaleStatus(File('test/fixtures/tailscale_status_running.json').readAsStringSync());

  /// Let real I/O run, and redraw, until `condition` holds.
  Future<void> until(WidgetTester tester, bool Function() condition, {int tries = 250}) async {
    for (var i = 0; i < tries; i++) {
      await tester.pump();
      if (condition()) return;
      await tester.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 20)));
    }
    throw TestFailure('the screen never got there');
  }

  Future<void> launch(WidgetTester tester, {FakePermissions? permissions, FakeService? service}) async {
    tester.view.physicalSize = const Size(1000, 800);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    await tester.runAsync(() async {
      app = testModel(tailnet, RecordingNotifier(), permissions: permissions, service: service);
      device = ChatPeer(
        identity: Identity.generate(),
        store: Store.open(':memory:'),
        name: 'thistle-46cf20',
        port: 0,
        loopback: true,
      );
      await device.start();
      await app.start();
    });
    await tester.pumpWidget(ThistleChatApp(model: app));
    await tester.pump();
  }

  /// The device writes first; the app accepts from Requests.
  Future<void> befriend(WidgetTester tester) async {
    device.addContact(app.listening!, name: 'laptop');
    device.sendText('laptop', 'hello from the device');
    await until(tester, () => app.requests.isNotEmpty);
    app.accept(app.requests.single.key);
    await until(tester, () => device.contact('laptop')!.acceptedByThem == true);
  }

  /// Stop both peers in real time, before the test's pending timers are
  /// counted: anything a tap started in the fake clock's zone ends with them.
  Future<void> finish(WidgetTester tester) async {
    await tester.pumpWidget(const SizedBox());
    await tester.runAsync(() async {
      await app.close();
      await device.stop();
    });
    // A WebSocket closed from a tap waits up to 5 s, on the fake clock, for
    // the other side's close.
    await tester.pump(const Duration(seconds: 10));
  }

  setUp(() => tailnet = TestTailnet(running()));

  testWidgets('says when Tailscale is not running, and why nothing connects', (tester) async {
    tailnet.status = const TailscaleStatus(TailnetState.notRunning);
    await launch(tester);
    expect(find.textContaining('Tailscale is not running'), findsOneWidget);

    tailnet.status = running();
    await tester.runAsync(app.refresh);
    await tester.pump();
    expect(find.textContaining('Tailscale is not running'), findsNothing);
    await finish(tester);
  });

  testWidgets('a request shows its name and fingerprint; Accept opens the conversation', (tester) async {
    await launch(tester);
    device.addContact(app.listening!, name: 'laptop');
    device.sendText('laptop', 'hello from the device');
    await until(tester, () => app.requests.isNotEmpty);

    await tester.tap(find.text('Requests'));
    await tester.pump();
    expect(find.text('thistle-46cf20'), findsOneWidget);
    expect(find.textContaining(device.identity.fingerprint), findsOneWidget);
    expect(find.textContaining('hello from the device'), findsOneWidget);

    await tester.tap(find.widgetWithText(FilledButton, 'Accept'));
    await tester.pump();
    expect(app.contacts.single.key, device.identity.keyHex);

    await tester.tap(find.text('Conversations'));
    await tester.pump();
    await tester.tap(find.text('thistle-46cf20'));
    await tester.pumpAndSettle();
    expect(find.text('hello from the device'), findsOneWidget);
    await finish(tester);
  });

  testWidgets('Decline removes the request', (tester) async {
    await launch(tester);
    device.addContact(app.listening!, name: 'laptop');
    device.sendText('laptop', 'buy now');
    await until(tester, () => app.requests.isNotEmpty);
    await tester.tap(find.text('Requests'));
    await tester.pump();
    await tester.tap(find.widgetWithText(OutlinedButton, 'Decline'));
    await tester.pump();
    expect(app.requests, isEmpty);
    expect(find.text('thistle-46cf20'), findsNothing);
    await until(tester, () => device.contact('laptop')!.declinedByThem);
    await finish(tester);
  });

  testWidgets('send, delivered, read; their typing; a reply and a reaction', (tester) async {
    await launch(tester);
    await befriend(tester);
    await tester.tap(find.text('thistle-46cf20'));
    await tester.pumpAndSettle();

    await tester.enterText(find.byKey(const Key('compose')), 'are you there?');
    await until(tester, () => device.isTyping(app.identity.keyHex));
    await tester.tap(find.byTooltip('Send'));
    await tester.pump();
    expect(find.text('are you there?'), findsOneWidget);
    await until(tester, () => find.text('Delivered').evaluate().isNotEmpty);
    device.markRead(app.identity.keyHex);
    await until(tester, () => find.text('Read').evaluate().isNotEmpty);

    device.setTyping(app.identity.keyHex, true);
    await until(tester, () => find.textContaining('typing').evaluate().isNotEmpty);

    // Reply to the device's message, and react to it.
    final first = app.messages(device.identity.keyHex).first.id;
    await tester.longPress(find.byKey(Key('message-$first')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Reply'));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('replying')), findsOneWidget);
    await tester.enterText(find.byKey(const Key('compose')), 'hello back');
    await tester.tap(find.byTooltip('Send'));
    await tester.pumpAndSettle();
    final reply = app.messages(device.identity.keyHex).last;
    expect(reply.text, 'hello back');
    expect(reply.replyTo, first);
    expect(find.byKey(Key('quote-${reply.id}')), findsOneWidget);

    await tester.longPress(find.byKey(Key('message-$first')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('👍'));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('reaction-me')), findsOneWidget);
    await until(
      tester,
      () => device.messages(app.identity.keyHex).firstWhere((m) => m.id == first).reactions['them'] == '👍',
    );

    // Their reaction shows too.
    device.react(app.identity.keyHex, reply.id, '❤');
    await until(tester, () => find.byKey(const Key('reaction-them')).evaluate().isNotEmpty);
    await finish(tester);
  });

  testWidgets('a changed key is shown as a warning, and can be accepted', (tester) async {
    await launch(tester);
    await befriend(tester);
    final port = device.address.port;
    await tester.runAsync(() async {
      await device.stop();
      device = ChatPeer(
        identity: Identity.generate(),
        store: Store.open(':memory:'),
        name: 'thistle-46cf20',
        port: port,
        loopback: true,
      );
      await device.start();
    });
    device.addContact(app.listening!, name: 'laptop');
    device.sendText('laptop', 'new phone who dis');
    await until(tester, () => app.contacts.single.flaggedKey != null);

    await tester.tap(find.text('Contacts'));
    await tester.pumpAndSettle();
    expect(find.textContaining('Key changed'), findsOneWidget);
    await tester.tap(find.text('thistle-46cf20'));
    await tester.pumpAndSettle();
    expect(find.textContaining(device.identity.fingerprint), findsOneWidget);
    await tester.tap(find.text('Accept the new key'));
    await tester.pumpAndSettle();
    expect(app.contacts.single.key, device.identity.keyHex);
    expect(find.textContaining('Key changed'), findsNothing);
    await finish(tester);
  });

  testWidgets('rename and remove a contact', (tester) async {
    await launch(tester);
    await befriend(tester);
    await tester.tap(find.text('Contacts'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('thistle-46cf20'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Rename'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('rename')), 'My T-Deck');
    await tester.tap(find.text('Save'));
    await tester.pumpAndSettle();
    expect(app.contacts.single.name, 'My T-Deck');
    expect(find.text('My T-Deck'), findsWidgets);

    await tester.tap(find.text('Remove'));
    await tester.pumpAndSettle();
    await tester.tap(find.widgetWithText(FilledButton, 'Remove'));
    await tester.pumpAndSettle();
    expect(app.contacts, isEmpty);
    await finish(tester);
  });

  testWidgets('discovered chat peers can be added, and a peer by name', (tester) async {
    tailnet.status = TailscaleStatus(
      TailnetState.running,
      selfName: 'laptop',
      selfIps: const ['100.91.10.1'],
      peers: const [
        TailnetPeer(hostName: 'thistle-46cf20', dnsName: 'thistle-46cf20', ips: ['127.0.0.1']),
        TailnetPeer(hostName: 'homelab', dnsName: 'homelab', ips: ['127.0.0.3']),
      ],
    );
    await launch(tester);
    // Probe the device's real port.
    await tester.runAsync(() async {
      await app.close();
      app = ChatModel(
        identity: Identity.generate(),
        openStore: () => Store.open(':memory:'),
        name: 'laptop',
        readStatus: tailnet.read,
        listenHost: '127.0.0.1',
        port: 0,
        loopback: true,
        discoveryPort: device.address.port,
        refreshEvery: Duration.zero,
      );
      await app.start();
    });
    await tester.pumpWidget(ThistleChatApp(model: app));
    await tester.tap(find.text('Contacts'));
    await tester.pump();
    await tester.tap(find.byTooltip('Look for chat peers'));
    await until(tester, () => app.discovered.isNotEmpty && !app.discovering);
    await tester.pump();
    expect(find.text('thistle-46cf20'), findsOneWidget);
    await tester.tap(find.widgetWithText(TextButton, 'Add'));
    await tester.pump();
    expect(app.contacts.single.name, 'thistle-46cf20');

    await tester.tap(find.byTooltip('Add by name or address'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('add-by-name')), 'homelab');
    await tester.tap(find.widgetWithText(FilledButton, 'Add'));
    await until(tester, () => app.contacts.length == 2);
    await tester.pumpAndSettle();
    expect(app.contacts.map((c) => c.address), contains(PeerAddress('127.0.0.3', device.address.port)));
    await finish(tester);
  });

  testWidgets('settings: our fingerprint, read receipts, background listening', (tester) async {
    await launch(tester);
    await tester.tap(find.text('Settings'));
    await tester.pumpAndSettle();
    expect(find.textContaining(app.identity.fingerprint), findsOneWidget);
    expect(find.textContaining('127.0.0.1'), findsOneWidget);
    await tester.tap(find.text('Send read receipts'));
    await tester.pump();
    expect(app.readReceipts, isFalse);
    await tester.tap(find.text('Listen in the background'));
    await tester.pump();
    expect(app.backgroundListening, isFalse);
    await finish(tester);
  });

  testWidgets('Android: says when the Tailscale app is not connected, and offers no discovery', (tester) async {
    tailnet.status = statusFromInterfaces({
      'wlan0': ['192.168.1.23'],
    });
    await launch(tester, permissions: FakePermissions(), service: FakeService());
    expect(find.textContaining('The Tailscale app is not connected'), findsOneWidget);

    tailnet.status = statusFromInterfaces({
      'tun0': ['100.99.0.7'],
    });
    await tester.runAsync(app.refresh);
    await tester.pump();
    expect(find.textContaining('Tailscale'), findsNothing);

    await tester.tap(find.text('Contacts'));
    await tester.pump();
    expect(find.byTooltip('Look for chat peers'), findsNothing);
    expect(find.byTooltip('Add by name or address'), findsOneWidget);
    expect(find.textContaining('Add a chat peer by its tailnet name'), findsOneWidget);
    await finish(tester);
  });

  testWidgets('a refused permission is explained, with the way to Android\'s settings', (tester) async {
    final permissions = FakePermissions()..answer = PermissionAnswer.permanentlyDenied;
    await launch(tester, permissions: permissions, service: FakeService());
    await until(tester, () => find.textContaining('Notifications are not allowed').evaluate().isNotEmpty);

    await tester.tap(find.text('Open settings'));
    await tester.pump();
    expect(permissions.settingsOpened, 1);

    await tester.tap(find.byTooltip('Dismiss'));
    await tester.pump();
    expect(find.textContaining('Notifications are not allowed'), findsNothing);
    await finish(tester);
  });

  /// Open the conversation with the device.
  Future<void> openDevice(WidgetTester tester) async {
    await tester.tap(find.text('Conversations'));
    await tester.pump();
    await tester.tap(find.text('thistle-46cf20'));
    await tester.pumpAndSettle();
  }

  testWidgets('a voice message shows its length and a play button; recording shows its time and can be thrown away', (
    tester,
  ) async {
    await launch(tester);
    await befriend(tester);
    final data = File('../../docs/tailnet-chat/vectors/voice.opus').readAsBytesSync();
    final id = device.sendVoice('laptop', data, 1500);
    await until(tester, () => app.messages(device.identity.keyHex).any((m) => m.id == id));
    await openDevice(tester);
    expect(find.byKey(Key('play-$id')), findsOneWidget);
    expect(find.text('Voice message, 0:01'), findsOneWidget);

    await tester.tap(find.byKey(const Key('record')));
    await until(tester, () => find.byKey(const Key('recording')).evaluate().isNotEmpty);
    expect(find.textContaining('Recording 0:0'), findsOneWidget);
    await tester.tap(find.byKey(const Key('recording-cancel')));
    await until(tester, () => find.byKey(const Key('recording')).evaluate().isEmpty);
    expect(find.byKey(const Key('compose')), findsOneWidget);
    await finish(tester);
  });

  testWidgets('Call opens the call screen; answered, it shows its time; Hang up ends it', (tester) async {
    await launch(tester);
    await befriend(tester); // the device answers calls by itself
    await openDevice(tester);
    await tester.tap(find.byKey(const Key('call')));
    await until(tester, () => app.talk.call?.state == 'connected');
    await tester.pump();
    expect(find.text('thistle-46cf20'), findsWidgets);
    expect(find.text('0:00'), findsOneWidget);
    expect(find.textContaining('Echo cancellation: none (test)'), findsOneWidget);
    await tester.tap(find.byKey(const Key('call-mute')));
    await tester.pump();
    expect(app.talk.muted, isTrue);
    await tester.tap(find.byKey(const Key('call-hangup')));
    await until(tester, () => find.text('Call ended').evaluate().isNotEmpty);
    await until(tester, () => device.calls.values.single.state == 'ended:hangup');
    await tester.tap(find.byKey(const Key('call-close')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('call-close')), findsNothing);
    await finish(tester);
  });

  testWidgets('a call coming in shows the call screen over the app; Answer connects it', (tester) async {
    await launch(tester);
    await befriend(tester);
    late String id;
    await tester.runAsync(() async => id = await device.call('laptop'));
    await until(tester, () => find.byKey(const Key('call-answer')).evaluate().isNotEmpty);
    expect(find.text('Calling you'), findsOneWidget);
    await tester.tap(find.byKey(const Key('call-answer')));
    await until(tester, () => device.calls[id]!.state == 'connected');
    await tester.runAsync(() async => device.endCall(id));
    await until(tester, () => find.text('Call ended').evaluate().isNotEmpty);
    await finish(tester);
  });
}
