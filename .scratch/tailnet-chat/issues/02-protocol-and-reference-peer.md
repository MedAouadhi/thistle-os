# 02: The protocol, its test vectors and the reference chat peer

**What to build:** Tailnet chat's protocol, version 1, written down as the spec's Implementation Decisions describe it (transport, admission, handshake, capabilities, `msg`, `ack`, `read`, `typing`, `react`, contact requests, voice messages, calls, binary frames, liveness, room for `webrtc` and `file`), with test vectors beside it and a stdlib Python reference chat peer that speaks it. Two reference chat peers on one machine can shake hands, send a contact request, accept it, exchange a message and ack it. The reference peer is what every later ticket's conformance tests drive.

**Blocked by:** None (can start immediately)

**Status:** done

- [x] The protocol document covers every message type, the handshake's signed transcript byte by byte, the binary frame header, error codes, and which rules are MUST and which SHOULD.
- [x] Test vectors: a handshake with fixed keys, nonces and signatures; one of each JSON message; frame headers; a short Ogg/Opus voice message and its 20 ms frames.
- [x] The reference chat peer, stdlib Python only (Ed25519 by a small pure-Python implementation if `cryptography` is absent), listens and dials, holds an identity key, a contacts list and an outbox, and can be scripted from a test.
- [x] Its own tests check it against the vectors and run two instances through handshake, contact request, accept and decline, a message, a duplicate, an unknown type answered with `error unsupported`, and a bad signature refused.
- [x] A conformance suite, parameterised by the target's address, holds those scenarios so later tickets can point it at the simulator and the companion core.

## Comments

2026-11-01: Done. `docs/tailnet-chat/protocol.md` is the protocol, `docs/tailnet-chat/vectors/` the vectors (`make_vectors.py --check` keeps them current), `tools/tailnet_chat/` the reference chat peer and the conformance suite, `docs/tailnet-chat/README.md` how to run them and the control protocol a target speaks over stdin/stdout. `tools/test_tailnet_chat.py` (42 tests, in CI's host tests) runs the conformance suite against an in-process peer, a peer process and an address alone.

Decisions the spec left open, now in the protocol: hex for keys, nonces, ids and signatures; the fingerprint is SHA-256's first 10 bytes; the transcript carries a role byte (01 dialler, 02 listener) after a 20-byte label, so the two signatures differ; the listener sends its `auth` only after verifying the dialler's, so a dialler knows it was accepted and a listener never signs for an unproven one; `react` and `contact` carry their own ids and go through the outbox; close codes 4000 (handshake), 4001 (duplicate), 4002 (key changed). Section 10 says what v1 does not stop: a tailnet node relaying a handshake.

`voice.opus` was made with the machine's FFmpeg 6.1.1 and libopus (recipe in `vectors/README.md`); the committed file is the vector. Calls are in the peer (answer, decline, busy, unreachable, audio frames) but not yet in the conformance suite; ticket 09 or 10 should add them there.
