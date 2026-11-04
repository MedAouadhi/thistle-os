import 'package:tailnet_chat_core/policy.dart';
import 'package:tailnet_chat_core/protocol.dart' show MediaState, SharedPeer;
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

    group('WebRTC between two video-capable chat peers', () {
      const v1 = ['call:ws-opus'];
      const both = ['call:ws-opus', 'call:webrtc'];

      test('the offer lists webrtc before ws-opus only when both hellos announce call:webrtc', () {
        expect(offerTransports(both, both), ['webrtc', 'ws-opus']);
        expect(offerTransports(both, v1), ['ws-opus']);
        expect(offerTransports(v1, both), ['ws-opus']);
        expect(offerTransports(v1, ['text']), isEmpty);
      });

      test('the answer picks webrtc whenever it is offered and the callee announced it', () {
        CallAnswer pick(List<String> transports, List<String> caps) =>
            answerOffer(isContact: true, busy: false, transports: transports, caps: caps, policy: CallPolicy.answer);
        expect(pick(['webrtc', 'ws-opus'], both).transport, 'webrtc');
        expect(pick(['ws-opus', 'webrtc'], both).transport, 'webrtc');
        expect(pick(['webrtc', 'ws-opus'], v1).transport, 'ws-opus', reason: 'the T-Deck');
        expect(pick(['webrtc'], v1).decline, 'unsupported');
        expect(pickTransport(['webrtc', 'ws-opus'], both), 'webrtc');
        expect(pickTransport(['ws-opus'], both), 'ws-opus');
        expect(pickTransport(['carrier-pigeon'], both), isNull);
      });

      test('a candidate is admitted only over UDP from an address in the tailnet\'s ranges', () {
        const v4 = 'candidate:1 1 udp 2122260223 100.91.111.54 47998 typ host generation 0';
        const v6 = 'candidate:2 1 UDP 2122262783 fd7a:115c:a1e0::3936:6f36 51234 typ host';
        expect(candidateAdmitted(v4), isTrue);
        expect(candidateAdmitted(v6), isTrue);
        expect(candidateAdmitted('a=$v4'), isTrue, reason: 'as an SDP writes it');
        expect(candidateAdmitted('candidate:3 1 udp 2122194687 192.168.2.131 40000 typ host'), isFalse);
        expect(
          candidateAdmitted('candidate:4 1 udp 1686052607 81.2.3.4 40000 typ srflx raddr 0.0.0.0 rport 0'),
          isFalse,
        );
        expect(candidateAdmitted('candidate:5 1 tcp 1518280447 100.91.111.54 9 typ host tcptype active'), isFalse);
        expect(candidateAdmitted('candidate:6 1 udp 2122260223 4f0c1a2e-1b9d.local 47998 typ host'), isFalse);
        expect(candidateAdmitted('candidate:7 1 udp 2122260223 100.128.0.1 47998 typ host'), isFalse);
        expect(candidateAdmitted(''), isFalse);
        expect(candidateAdmitted('nonsense'), isFalse);
      });

      test("an SDP keeps the tailnet's candidates and loses every other", () {
        const sdp =
            'v=0\r\n'
            'm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n'
            'a=candidate:1 1 udp 2122260223 100.91.111.54 47998 typ host\r\n'
            'a=candidate:2 1 udp 2122194687 192.168.2.131 40000 typ host\r\n'
            'a=candidate:3 1 tcp 1518280447 100.91.111.54 9 typ host tcptype active\r\n'
            'a=rtcp-mux\r\n';
        expect(
          tailnetOnlySdp(sdp),
          'v=0\r\n'
          'm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n'
          'a=candidate:1 1 udp 2122260223 100.91.111.54 47998 typ host\r\n'
          'a=rtcp-mux\r\n',
        );
        expect(tailnetOnlySdp('v=0\nm=audio 9 RTP/AVP 0\n'), 'v=0\nm=audio 9 RTP/AVP 0\n');
      });

      group('ends', () {
        final t0 = DateTime(2026, 11, 4, 12);
        String? due({required MediaState media, int answeredS = 0, int mediaS = 0, bool ever = false, int nowS = 0}) =>
            callEndDue(
              state: 'connected',
              since: t0.add(Duration(seconds: answeredS)),
              lastFrame: t0, // no ws-opus frames on a WebRTC call
              now: t0.add(Duration(seconds: nowS)),
              transport: 'webrtc',
              media: media,
              mediaSince: t0.add(Duration(seconds: mediaS)),
              mediaEverConnected: ever,
            );

        test('not connected 15 s after the answer ends as timeout', () {
          expect(due(media: MediaState.fresh, nowS: 15), isNull);
          expect(due(media: MediaState.fresh, nowS: 16), 'timeout');
          expect(due(media: MediaState.failed, mediaS: 3, nowS: 16), 'timeout', reason: 'never connected');
        });

        test('connected, it does not end however long no ws-opus frame comes', () {
          expect(due(media: MediaState.connected, mediaS: 1, ever: true, nowS: 600), isNull);
        });

        test('disconnected for 10 s after it connected ends as error, not before', () {
          expect(due(media: MediaState.disconnected, mediaS: 100, ever: true, nowS: 110), isNull);
          expect(due(media: MediaState.disconnected, mediaS: 100, ever: true, nowS: 111), 'error');
          expect(due(media: MediaState.failed, mediaS: 100, ever: true, nowS: 111), 'error');
        });

        test('ringing, the 45 s rule is unchanged', () {
          expect(
            callEndDue(
              state: 'ringing',
              since: t0,
              lastFrame: t0,
              now: t0.add(const Duration(seconds: 46)),
              transport: 'webrtc',
            ),
            'timeout',
          );
        });
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
