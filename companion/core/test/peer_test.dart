import 'dart:io';

import 'package:sqlite3/sqlite3.dart' show sqlite3;
import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:test/test.dart';

import 'vectors.dart';

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

Future<ChatPeer> startPeer(
  String name, {
  Identity? identity,
  String db = ':memory:',
  int port = 0,
  List<String> caps = capsV1,
}) async {
  final p = ChatPeer(
    caps: caps,
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

  test('a call to a peer that does not announce call:ws-opus ends at once as unsupported', () async {
    await bob.stop();
    bob = await startPeer('bob', caps: capsV1.where((c) => c != 'call:ws-opus').toList());
    await befriend(alice, bob);
    final call = await alice.call('bob');
    expect(alice.calls[call]!.state, 'ended:unsupported');
    await Future<void>.delayed(const Duration(milliseconds: 200));
    expect(bob.calls, isEmpty);
  });

  test("call audio comes out on the other side, in order, under its call", () async {
    await befriend(alice, bob);
    final heard = <CallAudio>[];
    final sub = bob.callAudio.listen(heard.add);
    final heardByAlice = <CallAudio>[];
    final sub2 = alice.callAudio.listen(heardByAlice.add);
    final call = await alice.call('bob');
    await eventually(() => alice.calls[call]!.state == 'connected', what: 'the answer');
    final frames = [for (final f in vector('voice.json')['frames'] as List) fromHex(f['hex'] as String)];
    for (var seq = 0; seq < 3; seq++) {
      alice.sendCallAudio(call, seq, frames[seq]);
    }
    bob.sendCallAudio(call, 0, frames[3]);
    await eventually(() => heard.length == 3 && heardByAlice.length == 1, what: 'the audio');
    expect([for (final a in heard) a.seq], [0, 1, 2]);
    expect(heard.every((a) => a.call == call), isTrue);
    expect(heard[2].packet, frames[2]);
    expect(heardByAlice.single.packet, frames[3]);
    expect(bob.calls[call]!.frames, 3);
    await sub.cancel();
    await sub2.cancel();
  });

  test('a call whose connection drops ends at once, as an error, the connection lost', () async {
    await befriend(alice, bob);
    final call = await alice.call('bob');
    await eventually(() => alice.calls[call]!.state == 'connected', what: 'the answer');
    await bob.stop();
    await eventually(() => alice.calls[call]!.state == 'ended:error', what: 'the call to end');
    expect(alice.calls[call]!.fault, CallFault.connectionLost);
  });

  group('WebRTC calls between video-capable peers', () {
    const video = [...capsV1, capWebrtc];
    final v = vector('webrtc.json');
    RtcCandidate candidate(int i) =>
        RtcCandidate((v['candidates'] as List)[i]['candidate'] as String, sdpMid: '0', sdpMLineIndex: 0);
    final sdp = v['sdp'] as Map<String, dynamic>;

    Future<String> webrtcCall({bool camera = false}) async {
      await alice.stop();
      await bob.stop();
      alice = await startPeer('alice', caps: video);
      bob = await startPeer('bob', caps: video);
      await befriend(alice, bob);
      final call = await alice.call('bob', video: camera);
      await eventually(() => alice.calls[call]!.state == 'connected', what: 'the answer');
      await eventually(() => bob.calls[call]?.state == 'connected', what: "bob's side");
      return call;
    }

    test('the call runs on webrtc, with the offer\'s video, and SDP and tailnet candidates go both ways', () async {
      final call = await webrtcCall(camera: true);
      expect(alice.calls[call]!.transport, 'webrtc');
      expect(bob.calls[call]!.transport, 'webrtc');
      expect(bob.calls[call]!.video, isTrue);
      final atBob = <CallRtc>[], atAlice = <CallRtc>[];
      final s1 = bob.callRtc.listen(atBob.add), s2 = alice.callRtc.listen(atAlice.add);
      expect(alice.sendRtc(call, RtcSdp('offer', sdp['written'] as String)), isTrue);
      expect(bob.sendRtc(call, candidate(2)), isFalse, reason: 'a LAN candidate is not sent');
      expect(bob.sendRtc(call, candidate(0)), isTrue);
      await eventually(() => atBob.isNotEmpty && atAlice.isNotEmpty, what: 'the signalling');
      expect(atBob.single.call, call);
      expect(atBob.single.payload.toJson(), {
        'sdp': {'type': 'offer', 'sdp': sdp['tailnet_only']},
      });
      expect(atAlice.single.payload.toJson(), candidate(0).toJson());
      await s1.cancel();
      await s2.cancel();
    });

    test("each side's camera state reaches the other, for its tile", () async {
      final call = await webrtcCall();
      expect(bob.setCamera(call, CameraState.paused), isTrue);
      await eventually(() => alice.calls[call]!.remoteCamera == CameraState.paused, what: 'the camera');
      alice.setCamera(call, CameraState.none);
      await eventually(() => bob.calls[call]!.remoteCamera == CameraState.none, what: 'the camera');
    });

    test('a contact is video-capable once its hello announced call:webrtc, and ours did too', () async {
      await alice.stop();
      await bob.stop();
      alice = await startPeer('alice', caps: video);
      bob = await startPeer('bob', caps: video);
      final tdeck = await startPeer('tdeck');
      addTearDown(tdeck.stop);
      expect(alice.videoCapable(bob.identity.keyHex), isFalse, reason: 'nothing heard yet');
      await befriend(alice, bob);
      await befriend(tdeck, alice); // the T-Deck writes first
      expect(alice.videoCapable(bob.identity.keyHex), isTrue);
      expect(bob.videoCapable(alice.identity.keyHex), isTrue);
      expect(alice.videoCapable(tdeck.identity.keyHex), isFalse, reason: 'the T-Deck announces no call:webrtc');
      expect(tdeck.videoCapable(alice.identity.keyHex), isFalse, reason: 'nor does it take part');
    });

    test('a contact stays video-capable across a restart, from its last hello, until one says otherwise', () async {
      await alice.stop();
      await bob.stop();
      final db = '${tmp.path}/alice.db', identity = Identity.generate();
      alice = await startPeer('alice', caps: video, db: db, identity: identity);
      bob = await startPeer('bob', caps: video);
      await befriend(alice, bob);
      final key = bob.identity.keyHex;
      expect(alice.videoCapable(key), isTrue);
      await alice.stop();
      alice = await startPeer('alice', caps: video, db: db, identity: identity);
      expect(alice.videoCapable(key), isTrue, reason: 'nothing heard yet this run: the stored hello says');
      // Bob's app goes back to a build without video: its next hello says so.
      await bob.stop();
      bob = await startPeer('bob', identity: bob.identity, port: bob.address.port);
      alice.sendText(key, 'still there?');
      await eventually(() => !alice.videoCapable(key), what: "bob's new hello");
      await alice.stop();
      alice = await startPeer('alice', caps: video, db: db, identity: identity);
      expect(alice.videoCapable(key), isFalse);
    });

    test('two phones, which gather no tailnet candidate, are not video-capable with each other and call on '
        'ws-opus; a phone and a laptop are, across a restart too', () async {
      const phone = [...video, capWebrtcPrflx];
      await alice.stop();
      await bob.stop();
      final db = '${tmp.path}/alice.db', identity = Identity.generate();
      alice = await startPeer('alice', caps: phone, db: db, identity: identity);
      bob = await startPeer('bob', caps: phone);
      final laptop = await startPeer('laptop', caps: video);
      addTearDown(laptop.stop);
      await befriend(alice, bob);
      await befriend(alice, laptop);
      expect(alice.videoCapable(bob.identity.keyHex), isFalse);
      expect(bob.videoCapable(alice.identity.keyHex), isFalse);
      expect(alice.videoCapable(laptop.identity.keyHex), isTrue);
      expect(laptop.videoCapable(alice.identity.keyHex), isTrue);
      final call = await alice.call('bob', video: true);
      await eventually(() => alice.calls[call]!.state == 'connected', what: 'the answer');
      expect(alice.calls[call]!.transport, 'ws-opus');
      expect(bob.calls[call]!.transports, ['ws-opus']);
      expect(bob.calls[call]!.video, isFalse);
      alice.endCall(call);
      await alice.stop();
      alice = await startPeer('alice', caps: phone, db: db, identity: identity);
      expect(alice.videoCapable(bob.identity.keyHex), isFalse, reason: "bob's stored hello");
      expect(alice.videoCapable(laptop.identity.keyHex), isTrue, reason: "the laptop's stored hello");
    });

    test("a database from before video keeps its contacts, none of them video-capable until it hears them", () {
      final path = '${tmp.path}/v1.db';
      sqlite3.open(path)
        ..execute('''
          CREATE TABLE contacts (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, key TEXT UNIQUE,
            host TEXT, port INTEGER, tailnet_name TEXT, flagged_key TEXT, accepted_by_them INTEGER,
            declined_by_them INTEGER NOT NULL DEFAULT 0);
          INSERT INTO contacts (name, key) VALUES ('bob', '${'b' * 64}');
          PRAGMA user_version = 1;
        ''')
        ..close();
      final store = Store.open(path);
      addTearDown(store.close);
      final bob = store.contactByKey('b' * 64)!;
      expect(bob.name, 'bob');
      expect(bob.webrtc, isFalse);
      store.setCallCaps(bob.id, [capWebrtc]);
      expect(store.contactByKey('b' * 64)!.webrtc, isTrue);
    });

    test("a database from before call:webrtc-prflx keeps each contact's call:webrtc, and learns the rest", () {
      final path = '${tmp.path}/v2.db';
      sqlite3.open(path)
        ..execute('''
          CREATE TABLE contacts (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, key TEXT UNIQUE,
            host TEXT, port INTEGER, tailnet_name TEXT, flagged_key TEXT, accepted_by_them INTEGER,
            declined_by_them INTEGER NOT NULL DEFAULT 0, webrtc INTEGER NOT NULL DEFAULT 0);
          INSERT INTO contacts (name, key, webrtc) VALUES ('bob', '${'b' * 64}', 1);
          PRAGMA user_version = 2;
        ''')
        ..close();
      final store = Store.open(path);
      addTearDown(store.close);
      final bob = store.contactByKey('b' * 64)!;
      expect(bob.callCaps, [capWebrtc]);
      store.setCallCaps(bob.id, [...capsV1, capWebrtc, capWebrtcPrflx]);
      expect(store.contactByKey('b' * 64)!.callCaps, [capWebrtc, capWebrtcPrflx]);
    });

    test('a peer without call:webrtc (the T-Deck) is offered ws-opus alone and takes it', () async {
      await alice.stop();
      alice = await startPeer('alice', caps: video);
      await befriend(alice, bob);
      final call = await alice.call('bob', video: true);
      await eventually(() => alice.calls[call]!.state == 'connected', what: 'the answer');
      expect(alice.calls[call]!.transport, 'ws-opus');
      expect(bob.calls[call]!.transports, ['ws-opus']);
      expect(bob.calls[call]!.video, isFalse);
    });

    test('media never connected 15 s after the answer ends the call as timeout, on both sides', () async {
      final call = await webrtcCall();
      alice.calls[call]!.since = DateTime.now().subtract(const Duration(seconds: 16));
      await eventually(() => alice.calls[call]!.state == 'ended:timeout', what: 'the end');
      await eventually(() => bob.calls[call]!.state == 'ended:timeout', what: "bob's end");
    });

    test('connected, no ws-opus frame is awaited; disconnected for 10 s, the call ends as error, media lost', () async {
      final call = await webrtcCall();
      alice.setMediaState(call, MediaState.connected);
      alice.calls[call]!.lastFrame = DateTime.now().subtract(const Duration(seconds: 30));
      await Future<void>.delayed(const Duration(milliseconds: 1500));
      expect(alice.calls[call]!.state, 'connected');
      alice.setMediaState(call, MediaState.disconnected);
      alice.calls[call]!.mediaSince = DateTime.now().subtract(const Duration(seconds: 11));
      await eventually(() => alice.calls[call]!.state == 'ended:error', what: 'the end');
      await eventually(() => bob.calls[call]!.state == 'ended:error', what: "bob's end");
      expect(alice.calls[call]!.fault, CallFault.mediaLost);
      expect(bob.calls[call]!.fault, CallFault.theirs, reason: 'the wire says error alone');
    });

    test('a call ended as an error says why on the side that ended it', () async {
      final call = await webrtcCall();
      alice.endCall(call, reason: 'error', fault: CallFault.offTailnet);
      await eventually(() => bob.calls[call]!.state == 'ended:error', what: "bob's end");
      expect(alice.calls[call]!.fault, CallFault.offTailnet);
    });
  });

  test('an incoming call nobody answered is missed; an answered one is not', () async {
    await befriend(alice, bob);
    bob.callPolicy = CallPolicy.ignore;
    final rang = await alice.call('bob');
    await eventually(() => bob.calls[rang]?.state == 'ringing', what: 'the ring');
    alice.endCall(rang);
    await eventually(() => bob.calls[rang]!.state == 'ended:hangup', what: 'the end');
    expect(bob.calls[rang]!.missed, isTrue);
    final answered = await alice.call('bob');
    await eventually(() => bob.calls[answered]?.state == 'ringing', what: 'the second ring');
    bob.answerCall(answered);
    await eventually(() => alice.calls[answered]!.state == 'connected', what: 'the answer');
    bob.endCall(answered);
    await eventually(() => alice.calls[answered]!.state == 'ended:hangup', what: 'the end');
    expect(bob.calls[answered]!.missed, isFalse);
    expect(alice.calls[answered]!.missed, isFalse);
  });

  test('a contact is told the tailnet peers, and a stranger none', () async {
    bob.tailnetPeers = () async => const [
      SharedPeer('homelab', ['100.64.0.2']),
    ];
    await befriend(alice, bob);
    expect(await alice.askPeers('bob'), [
      const SharedPeer('homelab', ['100.64.0.2']),
    ]);
    final carol = await startPeer('carol');
    addTearDown(carol.stop);
    carol.addContact(bob.address, name: 'bob');
    expect(await carol.askPeers('bob'), isEmpty);
  });

  test('a peer that does not announce peers is not asked', () async {
    final quiet = await startPeer('quiet', caps: [...capsV1.where((c) => c != 'peers')]);
    addTearDown(quiet.stop);
    final answered = <ChatEvent>[];
    quiet.events.where((e) => e.kind == 'answered').listen(answered.add);
    alice.addContact(quiet.address, name: 'quiet');
    expect(await alice.askPeers('quiet'), isNull);
    await Future<void>.delayed(const Duration(milliseconds: 200));
    expect(answered, isEmpty, reason: 'nothing was sent that it could not take');
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
