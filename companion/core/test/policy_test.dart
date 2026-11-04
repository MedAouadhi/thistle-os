import 'package:tailnet_chat_core/policy.dart';
import 'package:tailnet_chat_core/protocol.dart' show SharedPeer;
import 'package:test/test.dart';

const a = 'aa00000000000000000000000000000000000000000000000000000000000000';
const b = 'bb00000000000000000000000000000000000000000000000000000000000000';

Trust classifyFrom(
  List<ContactView> contacts, {
  String key = b,
  String peerName = 'laptop',
  bool dialler = false,
  PeerAddress? dialled,
  String remote = '127.0.0.1',
}) => classify(
  key: key,
  peerName: peerName,
  weDialled: dialler,
  dialled: dialled,
  remoteHost: remote,
  contacts: contacts,
);

void main() {
  group('trust', () {
    test('a key nobody pinned is a stranger', () {
      expect(classifyFrom([]), isA<Stranger>());
      expect(classifyFrom([const ContactView(id: 1, key: a, tailnetName: 'phone')]), isA<Stranger>());
    });

    test('a pinned key is its contact, whatever its name or address now', () {
      final t = classifyFrom([const ContactView(id: 7, key: b, tailnetName: 'old-name')], peerName: 'renamed');
      expect(t, isA<KnownContact>().having((t) => t.contact, 'contact', 7));
    });

    test('the first handshake with an address the user added pins its key', () {
      const addr = PeerAddress('100.100.1.2', 4810);
      final t = classifyFrom([const ContactView(id: 3, address: addr)], dialler: true, dialled: addr);
      expect(t, isA<PinKey>().having((t) => t.contact, 'contact', 3));
    });

    test('another key at a contact\'s dialled address is a mismatch', () {
      const addr = PeerAddress('100.100.1.2', 4810);
      final t = classifyFrom(
        [const ContactView(id: 3, key: a, address: addr)],
        dialler: true,
        dialled: addr,
      );
      expect(t, isA<KeyMismatch>().having((t) => t.contact, 'contact', 3));
    });

    test("another key under a contact's tailnet name is a mismatch", () {
      final t = classifyFrom([const ContactView(id: 4, key: a, tailnetName: 'laptop')], peerName: 'laptop');
      expect(t, isA<KeyMismatch>());
    });

    test("another key from a contact's tailnet source address is a mismatch; loopback says nothing", () {
      const addr = PeerAddress('100.100.1.2', 4810);
      expect(
        classifyFrom([const ContactView(id: 3, key: a, address: addr)], remote: '100.100.1.2'),
        isA<KeyMismatch>(),
      );
      const local = PeerAddress('127.0.0.1', 4810);
      expect(classifyFrom([const ContactView(id: 3, key: a, address: local)], remote: '127.0.0.1'), isA<Stranger>());
    });

    test('an incoming connection never pins an unpinned contact by its source address', () {
      const addr = PeerAddress('100.100.1.2', 4810);
      expect(classifyFrom([const ContactView(id: 3, address: addr)], remote: '100.100.1.2'), isA<Stranger>());
    });
  });

  group('the outbox', () {
    test('retries back off from 30 s, doubling, to 30 minutes, and never expire', () {
      expect([for (var i = 0; i < 9; i++) retryDelay(i).inSeconds], [30, 60, 120, 240, 480, 960, 1800, 1800, 1800]);
      expect(retryDelay(1000), const Duration(minutes: 30));
    });

    test('only messages, reactions, voice messages and contact answers are queued', () {
      for (final t in ['msg', 'react', 'voice', 'contact']) {
        expect(isQueued(t), isTrue, reason: t);
      }
      for (final t in ['read', 'typing', 'call_offer', 'call_answer', 'call_end', 'ack', 'error']) {
        expect(isQueued(t), isFalse, reason: t);
      }
    });

    test('a rejoin dials each contact with something waiting and each seen online', () {
      final contacts = [
        const ContactView(id: 1, key: a, tailnetName: 'laptop'),
        const ContactView(id: 2, key: b, tailnetName: 'phone'),
        const ContactView(id: 3, tailnetName: 'tv'),
        const ContactView(id: 4, key: '${a}x', tailnetName: 'away', declinedByThem: true),
      ];
      expect(toDial(contacts, waiting: {1, 4}, online: {'phone'}), [1, 2]);
    });
  });

  test('of two connections between the same keys, the one dialled by the lower key stays', () {
    expect(keepNewConnection(existingDialledBy: a, newDialledBy: b), isFalse);
    expect(keepNewConnection(existingDialledBy: b, newDialledBy: a), isTrue);
    // the same side dialled again: the old one is stale
    expect(keepNewConnection(existingDialledBy: a, newDialledBy: a), isTrue);
  });

  test('a feature is used only if both hellos announce it', () {
    expect(mayUse('typing', ['typing', 'text'], ['text', 'typing']), isTrue);
    expect(mayUse('receipts', ['receipts'], ['text']), isFalse);
  });

  test('typing lapses 6 s after the last on', () {
    final on = DateTime(2026, 11, 1, 12);
    expect(isTyping(on, on.add(const Duration(seconds: 5))), isTrue);
    expect(isTyping(on, on.add(const Duration(seconds: 6))), isFalse);
    expect(isTyping(null, on), isFalse);
  });

  test('a later reaction replaces an earlier one, never the reverse', () {
    expect(reactionReplaces(existingTs: null, newTs: 5), isTrue);
    expect(reactionReplaces(existingTs: 5, newTs: 6), isTrue);
    expect(reactionReplaces(existingTs: 6, newTs: 5), isFalse);
  });

  group('sharing the peer list', () {
    const online = [
      SharedPeer('homelab', ['100.64.0.2', 'fd7a:115c:a1e0::2']),
      SharedPeer('laptop', ['100.64.0.3']),
      SharedPeer('printer', ['192.168.1.20']),
    ];

    test("a contact is told the tailnet's online peers, less itself", () {
      expect(sharedPeers(online, asker: '100.64.0.3', contact: true), [
        const SharedPeer('homelab', ['100.64.0.2', 'fd7a:115c:a1e0::2']),
      ]);
      expect(sharedPeers(online, asker: '::ffff:100.64.0.2', contact: true), [
        const SharedPeer('laptop', ['100.64.0.3']),
      ]);
    });

    test('a stranger is told none', () {
      expect(sharedPeers(online, asker: '100.64.0.9', contact: false), isEmpty);
    });

    test('at most 64 peers, two tailnet addresses each, a name of 64 bytes, none unnamed', () {
      final many = [
        SharedPeer('é' * 40, ['192.168.1.2', '100.64.2.1', 'fd7a:115c:a1e0::21', '100.64.2.2']),
        const SharedPeer('', ['100.64.2.3']),
        for (var i = 0; i < 70; i++) SharedPeer('p$i', ['100.64.1.$i']),
      ];
      final shared = sharedPeers(many, contact: true);
      expect(shared, hasLength(64));
      expect(shared.first, SharedPeer('é' * 32, ['100.64.2.1', 'fd7a:115c:a1e0::21']));
      expect(shared[1].name, 'p0');
    });

    test('an asker probes only tailnet addresses, each once', () {
      const got = [
        SharedPeer('homelab', ['100.64.0.2', 'fd7a:115c:a1e0::2']),
        SharedPeer('evil', ['8.8.8.8', '127.0.0.1']),
        SharedPeer('again', ['100.64.0.2']),
        SharedPeer('junk', ['not an address']),
      ];
      expect(addressesToProbe(got), ['100.64.0.2', 'fd7a:115c:a1e0::2']);
    });
  });

  group('calls', () {
    CallAnswer answer({
      bool contact = true,
      bool busy = false,
      List<String> transports = const ['ws-opus'],
      CallPolicy policy = CallPolicy.answer,
    }) => answerOffer(
      isContact: contact,
      busy: busy,
      transports: transports,
      caps: const ['call:ws-opus'],
      policy: policy,
    );

    test('a contact\'s offer is answered with ws-opus', () {
      expect(answer().transport, 'ws-opus');
    });
    test('a stranger is declined, then a second call is busy, then an unknown transport unsupported', () {
      expect(answer(contact: false).decline, 'declined');
      expect(answer(busy: true).decline, 'busy');
      expect(answer(transports: ['webrtc']).decline, 'unsupported');
    });
    test('the user may decline or let it ring', () {
      expect(answer(policy: CallPolicy.decline).decline, 'declined');
      expect(answer(policy: CallPolicy.ignore).ringing, isTrue);
    });

    group('when a call ends by itself', () {
      final t0 = DateTime(2026, 11, 2, 12);
      String? due(String state, {int sinceS = 0, int frameS = 0, int nowS = 0}) => callEndDue(
        state: state,
        since: t0.add(Duration(seconds: sinceS)),
        lastFrame: t0.add(Duration(seconds: frameS)),
        now: t0.add(Duration(seconds: nowS)),
      );

      test('a connected call ends after 10 s without a frame from the other side, not before', () {
        expect(due('connected', frameS: 0, nowS: 10), isNull);
        expect(due('connected', frameS: 0, nowS: 11), 'timeout');
        expect(due('connected', sinceS: 0, frameS: 50, nowS: 59), isNull);
      });

      test('a call that rings unanswered for 45 s ends, on either side', () {
        for (final state in ['offered', 'ringing']) {
          expect(due(state, nowS: 45), isNull);
          expect(due(state, nowS: 46), 'timeout', reason: state);
        }
      });

      test('an ended or declined call has nothing more to end', () {
        expect(due('ended:hangup', nowS: 1000), isNull);
        expect(due('declined:busy', nowS: 1000), isNull);
      });
    });

    test('an incoming call never answered is missed; one answered, declined by us, or ours is not', () {
      expect(isMissedCall(mine: false, answered: false, state: 'ended:hangup'), isTrue);
      expect(isMissedCall(mine: false, answered: false, state: 'ended:timeout'), isTrue);
      expect(isMissedCall(mine: false, answered: false, state: 'declined:busy'), isTrue);
      expect(isMissedCall(mine: false, answered: false, state: 'ringing'), isFalse);
      expect(isMissedCall(mine: false, answered: false, state: 'declined:declined'), isFalse);
      expect(isMissedCall(mine: false, answered: true, state: 'ended:hangup'), isFalse);
      expect(isMissedCall(mine: true, answered: false, state: 'ended:timeout'), isFalse);
    });
  });
}
