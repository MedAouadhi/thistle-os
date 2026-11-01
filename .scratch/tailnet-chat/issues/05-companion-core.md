# 05: Companion core: a headless Dart chat peer that passes the conformance suite

**What to build:** the companion app's heart, as a pure Dart package with no Flutter dependency: identity key, listener and dialler, handshake, contacts and trust, messages, the outbox, receipts, typing, replies, reactions, and storage in a local database. A small command-line entry point runs it headless, and the reference chat peer's conformance suite passes against it as against the simulator.

**Blocked by:** 02

**Status:** done

- [x] The package lives in the repository's companion folder, reads the protocol's test vectors in its unit tests, and keeps its decisions (trust, outbox retries) in a module without I/O.
- [x] The headless entry point takes an identity file, a database path, an address and a port, and can be scripted by the conformance suite.
- [x] Every conformance scenario that tickets 03 and 04 run against the simulator passes against it.
- [x] The suite runs in CI's host tests alongside the simulator's.

## Comments

Done on `tailnet-chat-05-companion-core`.

- `companion/core/` is the package `tailnet_chat_core`: `protocol.dart` (the encodings, transcript, frame checks, admission) and `policy.dart` (trust, the outbox's backoff and what to dial on a rejoin, duplicate connections, capabilities, typing, reactions, the call rules) have no I/O; `peer.dart` is the listener and dialler on `dart:io`'s WebSocket, `store.dart` the SQLite database (contacts, requests, conversations, reactions, outbox, settings). Ed25519 is `ed25519_edwards` 0.3.2, SHA-256 `crypto`, SQLite `sqlite3` 3.8.0; versions pinned, `pubspec.lock` committed.
- `bin/tailnet_chat_core.dart` takes `--identity FILE --db PATH --address HOST --port N` (and `--loopback` for tests) and speaks the control protocol; `dart build cli` makes the binary CI runs the suite against.
- The unit tests read `handshake.json`, `messages.json`, `frames.json` and `voice.json`; others drive two cores through `ChatPeer`, and the headless binary across a restart (the key, a contact and the outbox kept).
- The suite had no scenarios for ticket 04's features, so they were added (`MESSAGE_SCENARIOS`: the outbox across a disconnect, order written, read receipts on and off, typing, replies, reactions), passing against the reference peer, `peer.py --control` and the core: 16 of 16. The reference peer now ignores a stranger's `typing`, as section 7 says.
- CI: a `companion-core` job in `tests.yml` (hosted, Dart 3.13.7 through `dart-lang/setup-dart@v1.7.1`): format, analyze, test, build, conformance.
- Beyond the ticket: call signalling (offer, answer, end, busy, unreachable, the 10 s timeout) and voice messages over the wire, since the suite's voice scenario needs them; audio itself is ticket 10's. `probe()` and `seenOnline()` are there for ticket 06's discovery.

