import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:thistle_chat/src/chat_model.dart';
import 'package:thistle_chat/src/permissions.dart';
import 'package:thistle_chat/src/tailnet.dart';

import 'helpers.dart';

void main() {
  late TestTailnet tailnet;
  late RecordingNotifier notifier;
  late ChatModel app;
  late ChatPeer other;

  setUp(() async {
    tailnet = TestTailnet(parseTailscaleStatus(File('test/fixtures/tailscale_status_running.json').readAsStringSync()));
    notifier = RecordingNotifier();
    app = testModel(tailnet, notifier);
    other = ChatPeer(
      identity: Identity.generate(),
      store: Store.open(':memory:'),
      name: 'thistle-46cf20',
      port: 0,
      loopback: true,
    );
    await other.start();
  });

  tearDown(() async {
    await app.close();
    await other.stop();
  });

  test('says Tailscale is not running, and listens once it runs', () async {
    tailnet.status = const TailscaleStatus(TailnetState.notRunning);
    await app.start();
    expect(app.problem, contains('Tailscale is not running'));
    expect(app.listening, isNull);

    tailnet.status = parseTailscaleStatus(File('test/fixtures/tailscale_status_running.json').readAsStringSync());
    await app.refresh();
    expect(app.problem, isNull);
    expect(app.listening!.host, '127.0.0.1');
  });

  test('a stranger is a contact request until accepted; then its messages show', () async {
    await app.start();
    app.foreground = false;
    other.addContact(app.listening!, name: 'laptop');
    other.sendText('laptop', 'hello from the device');
    await eventually(() => app.requests.isNotEmpty);

    final request = app.requests.single;
    expect(request.name, 'thistle-46cf20');
    expect(request.fingerprint, other.identity.fingerprint);
    expect(request.held.single.text, 'hello from the device');
    expect(notifier.shown.single.title, contains('thistle-46cf20'));

    app.accept(request.key);
    expect(app.requests, isEmpty);
    expect(app.contacts.single.key, other.identity.keyHex);
    expect(app.messages(request.key).single.text, 'hello from the device');
    await eventually(() => other.contact('laptop')!.acceptedByThem == true);
  });

  test('a declined request is gone, and the sender is told', () async {
    await app.start();
    other.addContact(app.listening!, name: 'laptop');
    other.sendText('laptop', 'hi');
    await eventually(() => app.requests.isNotEmpty);
    app.decline(app.requests.single.key);
    expect(app.requests, isEmpty);
    expect(app.contacts, isEmpty);
    await eventually(() => other.contact('laptop')!.declinedByThem);
  });

  group('with a contact', () {
    late String key;
    setUp(() async {
      await app.start();
      final c = app.addContact(other.address, name: 'Device');
      key = other.identity.keyHex;
      app.send(c.box, 'first');
      await eventually(() => other.requests().isNotEmpty);
      other.accept(app.identity.keyHex);
      await eventually(() => app.contacts.single.key == key);
    });

    test('a message goes from waiting to delivered, then read', () async {
      final id = app.send(key, 'are you there?');
      await eventually(() => app.messages(key).firstWhere((m) => m.id == id).state != 'waiting');
      expect(app.messages(key).firstWhere((m) => m.id == id).state, 'delivered');
      other.markRead(app.identity.keyHex);
      await eventually(() => app.messages(key).firstWhere((m) => m.id == id).state == 'read');
    });

    test('a reply quotes its message; a reaction shows, changes and goes', () async {
      final theirs = other.sendText(app.identity.keyHex, 'lunch?');
      await eventually(() => app.messages(key).any((m) => m.id == theirs));
      final reply = app.send(key, 'yes', replyTo: theirs);
      expect(app.messages(key).firstWhere((m) => m.id == reply).replyTo, theirs);
      expect(app.quoted(key, theirs)!.text, 'lunch?');

      app.react(key, theirs, '👍');
      expect(app.messages(key).firstWhere((m) => m.id == theirs).reactions['me'], '👍');
      await eventually(
        () => other.messages(app.identity.keyHex).firstWhere((m) => m.id == theirs).reactions['them'] == '👍',
      );
      app.react(key, theirs, '');
      expect(app.messages(key).firstWhere((m) => m.id == theirs).reactions, isNot(contains('me')));
    });

    test('their typing shows, and ours reaches them', () async {
      other.setTyping(app.identity.keyHex, true);
      await eventually(() => app.isTyping(key));
      app.typed(key, 'h');
      await eventually(() => other.isTyping(app.identity.keyHex));
    });

    test('an open conversation in front is read, and does not notify', () async {
      app.foreground = true;
      app.openConversation(key);
      notifier.shown.clear();
      final id = other.sendText(app.identity.keyHex, 'seen at once');
      await eventually(() => other.messages(app.identity.keyHex).firstWhere((m) => m.id == id).state == 'read');
      expect(notifier.shown, isEmpty);
    });

    test('in the background a message notifies', () async {
      app.foreground = false;
      notifier.shown.clear();
      other.sendText(app.identity.keyHex, 'are you up?');
      await eventually(() => notifier.shown.isNotEmpty);
      expect(notifier.shown.single.title, 'Device');
      expect(notifier.shown.single.body, 'are you up?');
    });

    test('read receipts off: they are not told', () async {
      app.readReceipts = false;
      app.foreground = true;
      app.openConversation(key);
      final id = other.sendText(app.identity.keyHex, 'unread for them');
      await eventually(() => app.messages(key).any((m) => m.id == id));
      await Future<void>.delayed(const Duration(milliseconds: 300));
      expect(other.messages(app.identity.keyHex).firstWhere((m) => m.id == id).state, 'delivered');
    });

    test('rename and remove', () async {
      app.rename(key, 'My T-Deck');
      expect(app.contacts.single.name, 'My T-Deck');
      app.remove(key);
      expect(app.contacts, isEmpty);
    });

    test('a changed key is flagged, and accepting it carries on', () async {
      // The device comes back with a new key at the same address.
      final port = other.address.port;
      await other.stop();
      other = ChatPeer(
        identity: Identity.generate(),
        store: Store.open(':memory:'),
        name: 'thistle-46cf20',
        port: port,
        loopback: true,
      );
      await other.start();
      other.addContact(app.listening!, name: 'laptop');
      other.sendText('laptop', 'it is me, reinstalled');
      await eventually(() => app.contacts.single.flaggedKey != null);
      expect(app.contacts.single.flaggedKey, other.identity.keyHex);
      await Future<void>.delayed(const Duration(milliseconds: 300));
      expect(app.requests, isEmpty);

      app.acceptNewKey(key);
      expect(app.contacts.single.key, other.identity.keyHex);
      expect(app.contacts.single.flaggedKey, isNull);
    });

    test('on Android, coming to the front dials the contacts known, as no peer list says who is online', () async {
      tailnet.status = statusFromInterfaces({
        'tun0': ['100.99.0.7'],
      });
      await app.refresh();
      // The device restarts, same key, same port: our connection to it is gone.
      final port = other.address.port;
      final id = other.identity;
      await other.stop();
      other = ChatPeer(identity: id, store: Store.open(':memory:'), name: 'thistle-46cf20', port: port, loopback: true);
      await other.start();
      await eventually(() => !app.connectedTo(key));

      app.foreground = false;
      await Future<void>.delayed(const Duration(milliseconds: 300));
      expect(app.connectedTo(key), isFalse);
      app.foreground = true;
      await eventually(() => app.connectedTo(key));
    });
  });

  test('discovery lists the tailnet peers that answer, and adds one', () async {
    tailnet.status = TailscaleStatus(
      TailnetState.running,
      selfName: 'laptop',
      selfIps: const ['100.91.10.1'],
      peers: const [
        TailnetPeer(hostName: 'thistle-46cf20', dnsName: 'thistle-46cf20', ips: ['127.0.0.1']),
        TailnetPeer(hostName: 'homelab', dnsName: 'homelab', ips: ['127.0.0.2']),
      ],
    );
    app = testModel(tailnet, notifier, discoveryPort: other.address.port);
    await app.start();
    await app.discover();
    expect(app.discovered.map((d) => d.peer.hostName), ['thistle-46cf20']);
    expect(app.discovered.single.probe.key, other.identity.keyHex);

    app.addDiscovered(app.discovered.single);
    expect(app.contacts.single.name, 'thistle-46cf20');
    expect(app.contacts.single.address, PeerAddress('127.0.0.1', other.address.port));
    // Added: no longer offered.
    expect(app.discovered, isEmpty);
  });

  test('a peer added by its tailnet name is dialled at its tailnet address', () async {
    await app.start();
    final c = await app.addByName('Thistle-46CF20');
    expect(c.address, const PeerAddress('100.110.0.38', 4810));
    final byIp = await app.addByName('127.0.0.1:${other.address.port}');
    expect(byIp.address, PeerAddress('127.0.0.1', other.address.port));
    await expectLater(app.addByName('no-such-host.invalid'), throwsA(isA<ArgumentError>()));
  });

  test('the background-listening setting is kept', () async {
    await app.start();
    expect(app.backgroundListening, isTrue);
    app.backgroundListening = false;
    expect(app.backgroundListening, isFalse);
  });

  group('the background service (Android)', () {
    late FakeService service;
    late FakePermissions permissions;
    setUp(() async {
      await app.close();
      service = FakeService();
      permissions = FakePermissions();
      app = testModel(tailnet, notifier, service: service, permissions: permissions);
    });

    test('on by default: started with the app, after notifications are asked for', () async {
      await app.start();
      await eventually(() => service.running);
      expect(permissions.asked, [AppPermission.notifications]);
    });

    test('switched off, it stops, and stays off at the next start', () async {
      final dir = Directory.systemTemp.createTempSync('thistle_chat_');
      addTearDown(() => dir.deleteSync(recursive: true));
      final db = '${dir.path}/chat.db';
      await app.close();
      app = testModel(tailnet, notifier, service: service, permissions: permissions, store: db);
      await app.start();
      app.backgroundListening = false;
      await eventually(() => !service.running);
      await app.close();

      final again = FakeService();
      app = testModel(tailnet, notifier, service: again, permissions: permissions, store: db);
      await app.start();
      await eventually(() => again.calls.isNotEmpty);
      expect(again.running, isFalse);
      expect(again.calls, [false]);
    });

    test('switched back on, it starts', () async {
      await app.start();
      app.backgroundListening = false;
      await eventually(() => !service.running);
      app.backgroundListening = true;
      await eventually(() => service.running);
    });

    test('notifications refused: the service still runs, and the refusal is explained', () async {
      permissions.answer = PermissionAnswer.denied;
      await app.start();
      await eventually(() => service.running);
      expect(app.permissions!.refusal(AppPermission.notifications), contains('Notifications are not allowed'));
    });
  });

  test('parsePeerInput', () {
    expect(parsePeerInput('homelab'), ('homelab', null));
    expect(parsePeerInput(' homelab:4811 '), ('homelab', 4811));
    expect(parsePeerInput('100.1.2.3'), ('100.1.2.3', null));
    expect(parsePeerInput('[fd7a::1]:4810'), ('fd7a::1', 4810));
    expect(parsePeerInput('fd7a:115c:a1e0::1'), ('fd7a:115c:a1e0::1', null));
    expect(() => parsePeerInput(''), throwsArgumentError);
  });
}
