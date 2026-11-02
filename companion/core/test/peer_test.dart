import 'dart:io';

import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:test/test.dart';

/// Two companion cores on loopback, driven through ChatPeer's public API.

Future<T> eventually<T>(
  T? Function() probe, {
  String what = 'a condition',
  Duration timeout = const Duration(seconds: 5),
}) async {
  final deadline = DateTime.now().add(timeout);
  while (true) {
    final v = probe();
    if (v != null && v != false && !(v is Iterable && v.isEmpty)) return v;
    if (DateTime.now().isAfter(deadline)) throw TestFailure('timed out waiting for $what');
    await Future<void>.delayed(const Duration(milliseconds: 20));
  }
}

Future<ChatPeer> startPeer(String name, {Identity? identity, String db = ':memory:', int port = 0}) async {
  final p = ChatPeer(
    identity: identity ?? Identity.generate(),
    store: Store.open(db),
    name: name,
    host: '127.0.0.1',
    port: port,
    loopback: true,
  );
  await p.start();
  return p;
}

/// Alice adds Bob by address and writes; Bob accepts. Both are contacts.
Future<void> befriend(ChatPeer alice, ChatPeer bob) async {
  alice.addContact(bob.address, name: 'bob');
  alice.sendText(bob.address, 'hi bob');
  await eventually(() => bob.store.request(alice.identity.keyHex), what: "alice's request");
  bob.accept(alice.identity.keyHex);
  await eventually(() => alice.contact('bob')!.acceptedByThem, what: '`contact accepted`');
}

void main() {
  late ChatPeer alice, bob;
  late Directory tmp;

  setUp(() async {
    tmp = await Directory.systemTemp.createTemp('tailnet_chat_core');
    alice = await startPeer('alice');
    bob = await startPeer('bob');
  });

  tearDown(() async {
    await alice.stop();
    await bob.stop();
    await tmp.delete(recursive: true);
  });

  test('a contact request accepted: pinned both ways, the message shown and delivered', () async {
    await befriend(alice, bob);
    final shown = bob.messages(alice.identity.keyHex);
    expect(shown.map((m) => m.text), ['hi bob']);
    expect(alice.contact('bob')!.key, bob.identity.keyHex);
    expect(alice.messages('bob').single.state, 'delivered');
    expect(alice.waiting('bob'), isEmpty);
  });

  test('a declined sender drops its outbox and sends no more', () async {
    alice.addContact(bob.address, name: 'bob');
    alice.sendText(bob.address, 'buy now');
    await eventually(() => bob.store.request(alice.identity.keyHex), what: 'the request');
    bob.decline(alice.identity.keyHex);
    await eventually(() => alice.contact('bob')!.declinedByThem, what: '`contact declined`');
    expect(() => alice.sendText('bob', 'again?'), throwsA(isA<Refused>()));
  });

  test('replies, reactions, typing and read receipts go both ways', () async {
    await befriend(alice, bob);
    final question = alice.messages('bob').single.id;
    final reply = bob.sendText(alice.identity.keyHex, 'hi alice', replyTo: question);
    final got = await eventually(
      () => alice.messages('bob').where((m) => m.id == reply).firstOrNull,
      what: 'the reply',
    );
    expect(got.replyTo, question);

    bob.react(alice.identity.keyHex, question, '👍');
    await eventually(() => alice.messages('bob').first.reactions['them'] == '👍', what: 'the reaction');
    bob.react(alice.identity.keyHex, question, '');
    await eventually(() => !alice.messages('bob').first.reactions.containsKey('them'), what: 'the reaction removed');

    alice.setTyping('bob', true);
    await eventually(() => bob.isTyping(alice.identity.keyHex), what: 'typing');
    alice.setTyping('bob', false);
    await eventually(() => !bob.isTyping(alice.identity.keyHex), what: 'typing off');

    alice.markRead('bob');
    await eventually(
      () => bob.messages(alice.identity.keyHex).firstWhere((m) => m.id == reply).state == 'read',
      what: 'the read receipt',
    );
  });

  test('read receipts switched off send none', () async {
    await befriend(alice, bob);
    bob.readReceipts = false;
    bob.markRead(alice.identity.keyHex);
    await Future<void>.delayed(const Duration(milliseconds: 300));
    expect(alice.messages('bob').single.state, 'delivered');
    expect(bob.messages(alice.identity.keyHex).single.state, 'read');
  });

  test('a message to a peer that is away waits, and is delivered when it is back; all of it kept on disk', () async {
    final aliceDb = '${tmp.path}/alice.db', bobDb = '${tmp.path}/bob.db';
    final aliceId = Identity.generate(), bobId = Identity.generate();
    await alice.stop();
    await bob.stop();
    alice = await startPeer('alice', identity: aliceId, db: aliceDb);
    bob = await startPeer('bob', identity: bobId, db: bobDb);
    await befriend(alice, bob);
    final port = bob.address.port;
    await bob.stop();

    final id = alice.sendText('bob', 'are you there?');
    expect(alice.waiting('bob'), [id]);

    // Alice restarts too: her outbox and contacts are in her database.
    await alice.stop();
    alice = await startPeer('alice', identity: aliceId, db: aliceDb);
    expect(alice.waiting('bob'), [id]);
    expect(alice.contact('bob')!.key, bobId.keyHex);

    bob = await startPeer('bob', identity: bobId, db: bobDb, port: port);
    await alice.flush();
    await eventually(() => alice.waiting('bob').isEmpty, what: 'the outbox to clear');
    expect(bob.messages(aliceId.keyHex).map((m) => m.text), ['hi bob', 'are you there?']);
    expect(alice.messages('bob').map((m) => m.state), ['delivered', 'delivered']);
  });

  test('a voice message goes through the outbox and arrives whole', () async {
    await befriend(alice, bob);
    final data = List<int>.generate(10000, (i) => i % 251);
    final id = alice.sendVoice('bob', data, 1500);
    final got = await eventually(
      () => bob.messages(alice.identity.keyHex).where((m) => m.id == id).firstOrNull,
      what: 'the voice message',
    );
    expect(got.voice, data);
    expect(got.ms, 1500);
    await eventually(() => alice.waiting('bob').isEmpty, what: 'the ack');
  });

  test('a call to a contact is answered and ended; a call to a peer that is away is unreachable', () async {
    await befriend(alice, bob);
    final call = await alice.call('bob');
    await eventually(() => alice.calls[call]!.state == 'connected', what: 'the answer');
    alice.endCall(call);
    await eventually(() => bob.calls[call]!.state == 'ended:hangup', what: 'the end');
    await bob.stop();
    final lost = await alice.call('bob');
    expect(alice.calls[lost]!.state, 'ended:unreachable');
  });

  test("a probe reads a chat peer's hello and leaves no request behind; nothing listening is null", () async {
    final hello = await alice.probe(bob.address);
    expect(hello!.key, bob.identity.keyHex);
    expect(hello.name, 'bob');
    expect(hello.fingerprint, bob.identity.fingerprint);
    await Future<void>.delayed(const Duration(milliseconds: 200));
    expect(bob.requests(), isEmpty);
    await bob.stop();
    expect(await alice.probe(bob.address), isNull);
  });

  test('a contact seen online in the peer list is dialled and its outbox flushed', () async {
    await befriend(alice, bob);
    final port = bob.address.port, bobId = bob.identity;
    await bob.stop();
    final id = alice.sendText('bob', 'when you are back');
    bob = await startPeer('bob', identity: bobId, port: port);
    await alice.seenOnline({'bob'});
    await eventually(() => alice.waiting('bob').isEmpty, what: 'the outbox to flush');
    expect(alice.messages('bob').last.id, id);
  });

  test("an impostor under a contact's tailnet name is flagged, and what it sends is not held as a request", () async {
    await befriend(alice, bob); // bob knows alice as tailnet name 'alice'
    final port = alice.address.port;
    await alice.stop();
    alice = await startPeer('alice', port: port); // a new key under the old name
    alice.addContact(bob.address, name: 'bob');
    alice.sendText(bob.address, 'it is me, honest');
    await eventually(() => bob.contacts().single.flaggedKey, what: 'the new key flagged');
    await Future<void>.delayed(const Duration(milliseconds: 300));
    expect(bob.requests(), isEmpty);
    expect(bob.contacts().single.flaggedKey, alice.identity.keyHex);
  });
}
