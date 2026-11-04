# 14: Sharing the tailnet's peer list, so Android can look for chat peers

**What to build:** the Android app cannot list the tailnet's peers (the Tailscale app shares none with other apps), so it could only add a chat peer by name or address. A chat peer that can list them, the Linux app or the device, now shares them with a contact that asks, and the Android app looks for chat peers among what its contacts share. Asked for by the user after the first phone run.

**Blocked by:** none (ticket 02's protocol, 05's core, 11's Android app)

**Status:** done (the phone run, Android asking the laptop and the T-Deck, to come)

### The protocol (version 1, a new capability; `docs/tailnet-chat/protocol.md`)

- A capability `peers`, used only when both hellos announce it (section 5).
- `{"type":"peers_ask"}`, answered with `{"type":"peers","peers":[{"name":"thistle-46cf20","addrs":["100.110.81.38","fd7a:115c:a1e0::2e38:5128"]}, ...]}`: the tailnet peers the answering side's platform shows online, less the asker. `name` is the tailnet hostname, at most 64 bytes; `addrs` 1 or 2 addresses inside the tailnet ranges (section 1). At most 64 peers. A peer with no list (Android) answers an empty one.
- Answered only to a **contact** (section 7). A stranger's `peers_ask` is answered with an empty `peers`; a peer that did not announce `peers`, or anything malformed, draws the usual `error` (section 4). An unasked `peers` is ignored.
- The asker MUST NOT take a key, a name or a trust decision from the list: it only learns addresses to probe with a hello on its discovery port, as Linux discovery does (spec, "Discovery"), and drops any address outside the tailnet ranges.
- `vectors/messages.json` gains valid and invalid frames; the reference peer (`tools/tailnet_chat/peer.py`) announces `peers`, answers from a list it is given (a command-line option and a control command), and can ask; `conformance.py` gains scenarios (answered to a contact, empty to a stranger, unannounced is unsupported, the limits), run against the reference peer both ways (`tools/test_tailnet_chat.py`), against the headless core and against the simulator.

### The device (`components/kernel_rs/src/tailnet_chat*.rs`)

- [x] Announces `peers` and answers a contact's `peers_ask` from the tailnet's online peers the kernel keeps (`tailnet_manager.rs`), less the asker's own address. The rules are host-tested (`tailnet_chat_wire.rs`, `tailnet_chat_policy.rs`).

### The companion (`companion/core/`, `companion/app/`)

- [x] The core announces `peers`, answers from a list the platform gives it (Linux: `tailscale status --json`'s online peers, as discovery reads them; Android: none), and has `askPeers(contact)`.
- [x] On Android the Contacts page gets **Look for chat peers**: it asks each contact that announces `peers` (dialling it if needed, a few seconds each), merges what they share, probes each address on the discovery port with a hello, and lists the chat peers found, as Linux does, each with the contact it came through ("via laptop"). Contacts and this device are left out. Add works as on Linux.
- [x] Dart tests for the core (`dart test`) and the app (`flutter test`); `flutter analyze` clean.

### Docs

- [x] `protocol.md` section 4 and 5, `docs/tailnet-chat/README.md` if it lists capabilities, the companion's README, CLAUDE.md's Tailnet chat lines.

## Comments

Done on `tailnet-chat-14-peers`, merged with ticket 13.

- **The protocol**: section 4.10 (`peers_ask`, `peers`), `peers` in section 5's list. Both frames need `peers` in both hellos, or draw `error unsupported` naming the type; neither is queued or acked. The answer is the platform's online peers less the one at the asker's address (an IPv4-mapped asker counts as its IPv4 address), each with its name cut to 64 bytes and at most two of its addresses inside the tailnet's ranges, a peer left with none dropped, 64 at most. A stranger is told `[]`. An unasked `peers` is ignored. Adding `peers` to the v1 caps changed the hellos in `handshake.json` (the signatures do not cover the caps).
- **Vectors**: `peers_ask`, `peers` and an empty `peers` among the valid frames; five invalid ones (not a list, 65 peers, a peer with no address, with three, a name over 64 bytes), all `bad_request` with `what` `peers`.
- **Reference peer** (`peer.py`): `tailnet_peers`, `--peers JSON`, the control commands `set_peers` (returns what a contact outside the list is told) and `ask_peers`; `ChatPeer.ask_peers`. The pure rules are `protocol.shared_peers` and `protocol.addresses_to_probe`.
- **Conformance** (`conformance.py`, `PEERS_SCENARIOS`): shared with a contact, empty to a stranger, unannounced is unsupported and an unasked list ignored, the limits, asked by the target. `Target.share_peers`/`ask_peers`; a target that never asks raises Skipped. Reference both ways: 30 of 30; the headless core: 29 of 30 (no cellular call); the simulator: every peers scenario but `peers_asked_by_target`, skipped as the device never asks.
- **Device**: announces `peers`; `tailnet_chat_policy::shared_peers` (host-tested: less the asker, IPv4-mapped, a stranger, the limits) over `tailnet_manager::peers()`'s online ones, by the first label of their names, IPv4 only, as the client knows no more; `wire::peers` written as the vector. The simulator's control port's `set_peers` answers with what the pretend tailnet shares, and `test_tailchat.py`'s conformance case checks the log's `told asker 2 tailnet peers` and `told nosy 0 tailnet peers`. The device does not ask.
- **Core** (Dart): `peers` in `capsV1`, `SharedPeer`, `ChatPeer.tailnetPeers` (an async callback the platform sets), `askPeers(who)` (null if either side does not announce `peers` or it cannot be reached), `sharedPeers` and `addressesToProbe` in `policy.dart`; `set_peers`/`ask_peers` in the control protocol.
- **App**: `TailscaleStatus.toShare` (the online peers by MagicDNS name; none on Android) answers a contact that asks. On Android Contacts now has Look for chat peers: `discoverThroughContacts` asks every contact at once (8 s at most each), probes each tailnet address shared once on the discovery port, skipping this phone's addresses and the contacts', and lists those that answer with "via <contact>", showing the address, not the shared name. This device's own key is left out too. Add is the same as on Linux.
- **Tests**: Python 52 (8 new), Rust 2243 (4 new), core `dart test` 83 (6 new), app `flutter test` 88 (5 new), simulator `test_tailchat.py` 12 of 12. `dart analyze` and `flutter analyze` clean; `flutter build apk --release --split-per-abi` builds.

Remaining, on hardware: Android's Look for chat peers asking the Linux app and the T-Deck (a contact each) over the tailnet, and Add on what they share.
