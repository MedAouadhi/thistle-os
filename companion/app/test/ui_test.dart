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
  FakeCallEngine? engine;

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

  Future<void> launch(
    WidgetTester tester, {
    FakePermissions? permissions,
    FakeService? service,
    TestCallWindow? callWindow,
    bool phone = false,
    bool videoCapable = false,
    List<String> cameras = const ['Integrated Webcam'],
  }) async {
    tester.view.physicalSize = phone ? const Size(390, 844) : const Size(1000, 800);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    await tester.runAsync(() async {
      app = testModel(
        tailnet,
        RecordingNotifier(),
        permissions: permissions,
        service: service,
        callEngine: engine = videoCapable ? FakeCallEngine(cameraList: cameras) : null,
      );
      device = ChatPeer(
        // Another companion app's core, when video-capable; else the T-Deck.
        caps: [...capsV1, if (videoCapable) capWebrtc],
        identity: Identity.generate(),
        store: Store.open(':memory:'),
        name: 'thistle-46cf20',
        port: 0,
        loopback: true,
      );
      await device.start();
      await app.start();
    });
    await tester.pumpWidget(ThistleChatApp(model: app, callWindow: callWindow));
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

  testWidgets('on a phone the chats are home: a request waits in a row over them, Accept puts it in them', (
    tester,
  ) async {
    await launch(tester, phone: true);
    expect(find.text('Chats'), findsOneWidget);
    expect(find.text('Find chat peers'), findsOneWidget);
    expect(find.text('New chat'), findsNothing);
    device.addContact(app.listening!, name: 'laptop');
    device.sendText('laptop', 'hello from the device');
    await until(tester, () => app.requests.isNotEmpty);

    await tester.tap(find.text('thistle-46cf20 wants to chat'));
    await tester.pumpAndSettle();
    expect(find.textContaining(device.identity.fingerprint), findsOneWidget);
    await tester.tap(find.widgetWithText(FilledButton, 'Accept'));
    await tester.pump();
    expect(find.text('No contact requests.'), findsOneWidget);
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();

    expect(find.text('thistle-46cf20 wants to chat'), findsNothing);
    expect(find.text('New chat'), findsOneWidget);
    await tester.tap(find.text('thistle-46cf20'));
    await tester.pumpAndSettle();
    expect(find.text('hello from the device'), findsOneWidget);
    await finish(tester);
  });

  testWidgets('on a phone New chat opens the contacts, and a contact its conversation in their place', (tester) async {
    await launch(tester, phone: true);
    await befriend(tester);
    await tester.pumpAndSettle(); // New chat comes in with the first contact
    await tester.tap(find.text('New chat'));
    await tester.pumpAndSettle();
    expect(find.byTooltip('Look for chat peers'), findsOneWidget);

    await tester.tap(find.byTooltip('About thistle-46cf20'));
    await tester.pumpAndSettle();
    expect(find.text('Rename'), findsOneWidget);
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();

    await tester.tap(find.text('thistle-46cf20'));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('compose')), findsOneWidget);
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    expect(find.text('Chats'), findsOneWidget);
    expect(find.byTooltip('Look for chat peers'), findsNothing);
    await finish(tester);
  });

  testWidgets('on a phone you, at the top, open Settings', (tester) async {
    await launch(tester, phone: true);
    await tester.tap(find.byTooltip('Settings'));
    await tester.pumpAndSettle();
    expect(find.textContaining(app.identity.fingerprint), findsOneWidget);
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    expect(find.text('Chats'), findsOneWidget);
    await finish(tester);
  });

  testWidgets('Android: says when the Tailscale app is not connected, and looks through the contacts', (tester) async {
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
    expect(find.byTooltip('Look for chat peers'), findsOneWidget);
    expect(find.byTooltip('Add by name or address'), findsOneWidget);
    expect(find.textContaining('Look for chat peers through your contacts'), findsOneWidget);
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

  testWidgets('Android: Settings says when calls cannot fill the screen, with the way to allow it', (tester) async {
    final permissions = FakePermissions()..fillScreen = false;
    await launch(tester, permissions: permissions, phone: true);
    await tester.tap(find.byTooltip('Settings'));
    await tester.pumpAndSettle();
    expect(find.text("Calls can't fill the screen"), findsOneWidget);
    await tester.tap(find.text('Allow'));
    await tester.pump();
    expect(permissions.fillScreenOpened, 1);
    await finish(tester);
  });

  testWidgets('Settings says nothing of the full screen where calls may fill it', (tester) async {
    await launch(tester, permissions: FakePermissions(), phone: true);
    await tester.tap(find.byTooltip('Settings'));
    await tester.pumpAndSettle();
    expect(find.text("Calls can't fill the screen"), findsNothing);
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

  testWidgets("a call with another companion app is WebRTC, and the echo line names WebRTC's canceller", (
    tester,
  ) async {
    await launch(tester, videoCapable: true);
    await befriend(tester);
    await openDevice(tester);
    await tester.tap(find.byKey(const Key('call')));
    await until(tester, () => app.talk.call?.state == 'connected' && app.talk.echoCancellation != null);
    await tester.pump(const Duration(seconds: 1)); // the call screen has slid in
    expect(app.talk.call!.transport, 'webrtc');
    expect(find.textContaining('Echo cancellation: on (WebRTC, fake)'), findsOneWidget);
    await tester.tap(find.byKey(const Key('call-hangup')));
    await until(tester, () => device.calls.values.single.state == 'ended:hangup');
    await until(tester, () => find.byKey(const Key('call-close')).evaluate().isNotEmpty);
    await tester.tap(find.byKey(const Key('call-close')));
    await tester.pumpAndSettle();
    await finish(tester);
  });

  testWidgets('a call coming in shows the call screen over the app; Answer connects it', (tester) async {
    await launch(tester);
    await befriend(tester);
    late String id;
    await tester.runAsync(() async => id = await device.call('laptop'));
    await until(tester, () => find.byKey(const Key('call-answer')).evaluate().isNotEmpty);
    expect(find.text('Calling you'), findsOneWidget);
    expect(find.byKey(const Key('call-answer-video')), findsNothing, reason: 'two buttons, no camera asked');
    await tester.tap(find.byKey(const Key('call-answer')));
    await until(tester, () => device.calls[id]!.state == 'connected');
    await tester.runAsync(() async => device.endCall(id));
    await until(tester, () => find.text('Call ended').evaluate().isNotEmpty);
    await finish(tester);
  });

  testWidgets('Android: the call window shows the call alone; ended, it finishes', (tester) async {
    final window = TestCallWindow();
    await launch(tester, callWindow: window, phone: true);
    await befriend(tester);
    expect(find.text('thistle-46cf20'), findsOneWidget, reason: 'the chats, before');
    late String id;
    await tester.runAsync(() async => id = await device.call('laptop'));
    await until(tester, () => app.talk.call?.state == 'ringing');
    window.show();
    await until(tester, () => find.byKey(const Key('call-answer')).evaluate().isNotEmpty);
    expect(find.text('hello from the device'), findsNothing);
    expect(find.byTooltip('Settings'), findsNothing);
    await tester.tap(find.byKey(const Key('call-answer')));
    await until(tester, () => device.calls[id]!.state == 'connected');
    await tester.runAsync(() async => device.endCall(id));
    await until(tester, () => find.text('Call ended').evaluate().isNotEmpty);
    expect(window.finished, 0);
    await tester.pump(const Duration(seconds: 4));
    expect(window.finished, 1);
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('call-status')), findsNothing, reason: 'the chats again, once unlocked');
    await finish(tester);
  });

  group('video', () {
    /// Hang up and close the call screen, as the user would.
    Future<void> hangUp(WidgetTester tester) async {
      await tester.pump(const Duration(seconds: 1)); // the call screen has slid in
      if (app.talk.call?.live ?? false) await tester.tap(find.byKey(const Key('call-hangup')));
      await until(tester, () => find.byKey(const Key('call-close')).evaluate().isNotEmpty);
      await tester.tap(find.byKey(const Key('call-close')));
      await tester.pumpAndSettle();
    }

    testWidgets('a conversation with a video-capable contact has a Video call button beside Call', (tester) async {
      await launch(tester, videoCapable: true);
      await befriend(tester);
      await openDevice(tester);
      expect(find.byKey(const Key('call')), findsOneWidget);
      expect(find.byKey(const Key('video-call')), findsOneWidget);
      await finish(tester);
    });

    testWidgets('the T-Deck, which cannot do video, has no Video call button', (tester) async {
      await launch(tester);
      await befriend(tester);
      await openDevice(tester);
      expect(find.byKey(const Key('call')), findsOneWidget);
      expect(find.byKey(const Key('video-call')), findsNothing);
      await finish(tester);
    });

    /// A call from the app, answered by the device's core.
    Future<String> placed(
      WidgetTester tester, {
      String button = 'call',
      List<String> cameras = const ['Integrated Webcam'],
    }) async {
      await launch(tester, videoCapable: true, cameras: cameras);
      await befriend(tester);
      await openDevice(tester);
      await tester.tap(find.byKey(Key(button)));
      await until(tester, () => app.talk.call?.state == 'connected' && app.talk.cameras != null);
      return app.talk.callId!;
    }

    testWidgets('Video call rings with video and our camera on, its picture in a corner', (tester) async {
      final id = await placed(tester, button: 'video-call');
      expect(device.calls[id]!.video, isTrue);
      await until(tester, () => device.calls[id]!.remoteCamera == CameraState.on);
      expect(find.byKey(const Key('fake-local-video')), findsOneWidget);
      expect(engine!.session.camera, 'Integrated Webcam');
      await hangUp(tester);
      await finish(tester);
    });

    testWidgets("the other side's tile follows its camera: on, off, paused, none", (tester) async {
      final id = await placed(tester);
      expect(find.byKey(const Key('fake-remote-video')), findsNothing);
      for (final (camera, says) in [
        (CameraState.on, null),
        (CameraState.paused, 'Camera paused'),
        (CameraState.on, null),
        (CameraState.off, 'Camera off'),
        (CameraState.none, 'No camera'),
      ]) {
        device.setCamera(id, camera);
        await until(tester, () => app.talk.call!.remoteCamera == camera);
        await tester.pump();
        expect(find.byKey(const Key('fake-remote-video')), says == null ? findsOneWidget : findsNothing);
        if (says != null) expect(find.text(says), findsOneWidget);
      }
      await hangUp(tester);
      await finish(tester);
    });

    testWidgets('the camera switch turns ours on and off; with one camera there is no Switch camera', (tester) async {
      final id = await placed(tester);
      expect(find.byKey(const Key('fake-local-video')), findsNothing, reason: 'Call keeps the camera off');
      await until(tester, () => device.calls[id]!.remoteCamera == CameraState.off);
      expect(find.byKey(const Key('call-switch-camera')), findsNothing);
      await tester.tap(find.byKey(const Key('call-camera')));
      await until(tester, () => device.calls[id]!.remoteCamera == CameraState.on);
      await tester.pump();
      expect(find.byKey(const Key('fake-local-video')), findsOneWidget);
      await tester.tap(find.byKey(const Key('call-camera')));
      await until(tester, () => device.calls[id]!.remoteCamera == CameraState.off);
      await tester.pump();
      expect(find.byKey(const Key('fake-local-video')), findsNothing);
      await hangUp(tester);
      await finish(tester);
    });

    testWidgets('Switch camera, with two cameras, goes to the other', (tester) async {
      await placed(tester, button: 'video-call', cameras: const ['Integrated Webcam', 'USB Camera']);
      await until(tester, () => find.byKey(const Key('call-switch-camera')).evaluate().isNotEmpty);
      await tester.tap(find.byKey(const Key('call-switch-camera')));
      await until(tester, () => engine!.session.camera == 'USB Camera');
      await tester.tap(find.byKey(const Key('call-switch-camera')));
      await until(tester, () => engine!.session.camera == 'Integrated Webcam');
      await hangUp(tester);
      await finish(tester);
    });

    testWidgets('without a camera the switch says No camera and cannot be turned on', (tester) async {
      final id = await placed(tester, cameras: const []);
      await until(tester, () => device.calls[id]!.remoteCamera == CameraState.none);
      await tester.pump();
      expect(find.text('No camera'), findsOneWidget);
      await tester.tap(find.byKey(const Key('call-camera')), warnIfMissed: false);
      await tester.pump();
      expect(engine!.session.cameraLog, isEmpty);
      await hangUp(tester);
      await finish(tester);
    });

    testWidgets('a video call coming in: Answer with video, Answer and Decline; Answer leaves the camera off', (
      tester,
    ) async {
      await launch(tester, videoCapable: true);
      await befriend(tester);
      late String id;
      await tester.runAsync(() async => id = await device.call('laptop', video: true));
      await until(tester, () => find.byKey(const Key('call-answer')).evaluate().isNotEmpty);
      expect(find.text('Video call'), findsOneWidget);
      expect(find.byKey(const Key('call-answer-video')), findsOneWidget);
      expect(find.byKey(const Key('call-decline')), findsOneWidget);
      await tester.tap(find.byKey(const Key('call-answer')));
      await until(tester, () => device.calls[id]!.remoteCamera == CameraState.off);
      expect(engine!.session.cameraLog, isEmpty, reason: 'never on without a tap');
      await hangUp(tester);
      await finish(tester);
    });

    testWidgets('Answer with video turns the camera on', (tester) async {
      await launch(tester, videoCapable: true);
      await befriend(tester);
      late String id;
      await tester.runAsync(() async => id = await device.call('laptop', video: true));
      await until(tester, () => find.byKey(const Key('call-answer-video')).evaluate().isNotEmpty);
      await tester.tap(find.byKey(const Key('call-answer-video')));
      await until(tester, () => device.calls[id]!.remoteCamera == CameraState.on);
      await hangUp(tester);
      await finish(tester);
    });
  });
}
