import 'dart:convert';

import 'package:tailnet_chat_core/policy.dart' show candidateAdmitted, offerTransports, tailnetOnlySdp;
import 'package:tailnet_chat_core/protocol.dart';
import 'package:test/test.dart';

import 'vectors.dart';

void main() {
  group('the handshake vector', () {
    final v = vector('handshake.json');
    final d = v['dialler'] as Map<String, dynamic>;
    final l = v['listener'] as Map<String, dynamic>;
    final transcripts = v['transcripts'] as Map<String, dynamic>;

    test('a seed gives its key and fingerprint', () {
      for (final side in [d, l]) {
        final id = Identity.fromSeed(fromHex(side['seed'] as String));
        expect(id.keyHex, side['key']);
        expect(id.fingerprint, side['fingerprint']);
      }
    });

    test("both transcripts are the vector's bytes", () {
      for (final role in Role.values) {
        final t = transcript(
          role,
          fromHex(d['nonce'] as String),
          fromHex(l['nonce'] as String),
          fromHex(d['key'] as String),
          fromHex(l['key'] as String),
        );
        expect(t.length, 149);
        expect(toHex(t), transcripts[role.name]);
      }
    });

    test('each side signs its auth as the vector does, and the other verifies it', () {
      for (final (side, role) in [(d, Role.dialler), (l, Role.listener)]) {
        final id = Identity.fromSeed(fromHex(side['seed'] as String));
        final t = fromHex(transcripts[role.name] as String);
        final auth = jsonDecode(side['auth'] as String) as Map<String, dynamic>;
        expect(toHex(id.sign(t)), auth['sig']);
        expect(verifySignature(fromHex(side['key'] as String), t, fromHex(auth['sig'] as String)), isTrue);
      }
    });

    test('the hellos are what helloFrame() writes', () {
      for (final side in [d, l]) {
        final id = Identity.fromSeed(fromHex(side['seed'] as String));
        final hello = jsonDecode(side['hello'] as String);
        expect(helloFrame(id.key, side['name'] as String, capsV1, fromHex(side['nonce'] as String)), hello);
        expect(
          encodeFrame(helloFrame(id.key, side['name'] as String, capsV1, fromHex(side['nonce'] as String))),
          side['hello'],
        );
        expect(checkFrame(side['hello'] as String).frame, hello);
      }
    });

    test('a listener refuses each refused auth, and a zero signature', () {
      final t = fromHex(transcripts['dialler'] as String);
      final key = fromHex(d['key'] as String);
      for (final r in (v['refused'] as List).cast<Map<String, dynamic>>()) {
        final sig = fromHex((jsonDecode(r['auth'] as String) as Map)['sig'] as String);
        expect(verifySignature(key, t, sig), isFalse, reason: r['why'] as String);
      }
      expect(verifySignature(key, t, List.filled(64, 0)), isFalse);
    });
  });

  group('the message vectors', () {
    final v = vector('messages.json');

    test('every valid frame checks', () {
      for (final f in (v['valid'] as List).cast<Map<String, dynamic>>()) {
        final r = checkFrame(f['text'] as String);
        expect(r.error, isNull, reason: f['name'] as String);
        expect(r.frame, jsonDecode(f['text'] as String));
      }
    });

    test('every invalid frame draws its error', () {
      for (final f in (v['invalid'] as List).cast<Map<String, dynamic>>()) {
        final r = checkFrame(f['text'] as String);
        final want = f['error'] as Map<String, dynamic>;
        expect(r.frame, isNull, reason: f['name'] as String);
        expect(r.error!['code'], want['code'], reason: f['name'] as String);
        for (final name in ['what', 'id']) {
          if (want.containsKey(name)) expect(r.error![name], want[name], reason: f['name'] as String);
        }
      }
    });

    test('a frame over 16384 bytes is too large', () {
      final big = jsonEncode({'type': 'typing', 'on': true, 'pad': 'x' * 16400});
      expect(checkFrame(big).error!['code'], 'too_large');
    });

    test('members in another order, and unknown ones, still check', () {
      final r = checkFrame('{"on": false, "future": [1], "type": "typing"}');
      expect(r.frame!['on'], false);
    });
  });

  group('the WebRTC vectors', () {
    final v = vector('webrtc.json');

    test('every valid frame checks, and every invalid one draws its error', () {
      for (final f in (v['valid'] as List).cast<Map<String, dynamic>>()) {
        final r = checkFrame(f['text'] as String);
        expect(r.error, isNull, reason: f['name'] as String);
      }
      for (final f in (v['invalid'] as List).cast<Map<String, dynamic>>()) {
        final r = checkFrame(f['text'] as String);
        final want = f['error'] as Map<String, dynamic>;
        expect(r.error?['code'], want['code'], reason: f['name'] as String);
        expect(r.error?['what'], want['what'], reason: f['name'] as String);
      }
    });

    test('each candidate is admitted or not as the vector says, and the SDP keeps the tailnet\'s', () {
      for (final c in (v['candidates'] as List).cast<Map<String, dynamic>>()) {
        expect(candidateAdmitted(c['candidate'] as String), c['admitted'], reason: c['candidate'] as String);
      }
      expect(tailnetOnlySdp(v['sdp']['written'] as String), v['sdp']['tailnet_only']);
    });

    test('an offer lists webrtc unless both sides gather no tailnet candidate (two phones)', () {
      for (final o in (v['offers'] as List).cast<Map<String, dynamic>>()) {
        final caller = (o['caller'] as List).cast<String>(), callee = (o['callee'] as List).cast<String>();
        expect(offerTransports(caller, callee), o['transports'], reason: o['name'] as String);
      }
    });
  });

  group('the binary frame vectors', () {
    final v = vector('frames.json');

    test('each valid frame packs and unpacks', () {
      for (final f in (v['valid'] as List).cast<Map<String, dynamic>>()) {
        final packed = packBinary(
          f['kind'] as int,
          f['stream'] as int,
          f['seq'] as int,
          fromHex(f['payload'] as String),
        );
        expect(toHex(packed), f['frame']);
        final b = unpackBinary(fromHex(f['frame'] as String));
        expect([b.kind, b.stream, b.seq, toHex(b.payload)], [f['kind'], f['stream'], f['seq'], f['payload']]);
      }
    });

    test('each malformed frame is refused', () {
      for (final f in (v['invalid'] as List).cast<Map<String, dynamic>>()) {
        expect(() => unpackBinary(fromHex(f['frame'] as String)), throwsFormatException, reason: f['why'] as String);
      }
    });
  });

  test("the voice vector's chunks add up to its file", () {
    final v = vector('voice.json');
    final voice = checkFrame(v['message'] as String).frame!;
    var total = 0;
    for (final (i, c) in (v['chunks'] as List).cast<Map<String, dynamic>>().indexed) {
      final b = unpackBinary(fromHex(c['frame'] as String));
      expect([b.kind, b.stream, b.seq], [kindVoice, voice['stream'], i]);
      expect(b.payload.length, lessThanOrEqualTo(maxChunk));
      total += b.payload.length;
    }
    expect(total, voice['bytes']);
  });

  test("admission: the tailnet's ranges, and loopback only where allowed", () {
    for (final (addr, loop, want) in [
      ('100.64.0.1', false, true),
      ('100.127.255.254', false, true),
      ('100.128.0.1', false, false),
      ('100.63.255.255', false, false),
      ('192.168.1.10', false, false),
      ('fd7a:115c:a1e0::1', false, true),
      ('fd7a:115c:a1e1::1', false, false),
      ('::ffff:100.100.1.1', false, true),
      ('127.0.0.1', false, false),
      ('127.0.0.1', true, true),
      ('::1', true, true),
      ('not an address', true, false),
    ]) {
      expect(admitted(addr, loopback: loop), want, reason: '$addr loopback=$loop');
    }
  });
}
