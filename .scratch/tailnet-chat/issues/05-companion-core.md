# 05: Companion core: a headless Dart chat peer that passes the conformance suite

**What to build:** the companion app's heart, as a pure Dart package with no Flutter dependency: identity key, listener and dialler, handshake, contacts and trust, messages, the outbox, receipts, typing, replies, reactions, and storage in a local database. A small command-line entry point runs it headless, and the reference chat peer's conformance suite passes against it as against the simulator.

**Blocked by:** 02

**Status:** ready-for-agent

- [ ] The package lives in the repository's companion folder, reads the protocol's test vectors in its unit tests, and keeps its decisions (trust, outbox retries) in a module without I/O.
- [ ] The headless entry point takes an identity file, a database path, an address and a port, and can be scripted by the conformance suite.
- [ ] Every conformance scenario that tickets 03 and 04 run against the simulator passes against it.
- [ ] The suite runs in CI's host tests alongside the simulator's.
