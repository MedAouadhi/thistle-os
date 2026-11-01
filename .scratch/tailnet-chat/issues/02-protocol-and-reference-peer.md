# 02: The protocol, its test vectors and the reference chat peer

**What to build:** Tailnet chat's protocol, version 1, written down as the spec's Implementation Decisions describe it (transport, admission, handshake, capabilities, `msg`, `ack`, `read`, `typing`, `react`, contact requests, voice messages, calls, binary frames, liveness, room for `webrtc` and `file`), with test vectors beside it and a stdlib Python reference chat peer that speaks it. Two reference chat peers on one machine can shake hands, send a contact request, accept it, exchange a message and ack it. The reference peer is what every later ticket's conformance tests drive.

**Blocked by:** None (can start immediately)

**Status:** ready-for-agent

- [ ] The protocol document covers every message type, the handshake's signed transcript byte by byte, the binary frame header, error codes, and which rules are MUST and which SHOULD.
- [ ] Test vectors: a handshake with fixed keys, nonces and signatures; one of each JSON message; frame headers; a short Ogg/Opus voice message and its 20 ms frames.
- [ ] The reference chat peer, stdlib Python only (Ed25519 by a small pure-Python implementation if `cryptography` is absent), listens and dials, holds an identity key, a contacts list and an outbox, and can be scripted from a test.
- [ ] Its own tests check it against the vectors and run two instances through handshake, contact request, accept and decline, a message, a duplicate, an unknown type answered with `error unsupported`, and a bad signature refused.
- [ ] A conformance suite, parameterised by the target's address, holds those scenarios so later tickets can point it at the simulator and the companion core.
