# 14: Sharing the tailnet's peer list, so Android can look for chat peers

**What to build:** the Android app cannot list the tailnet's peers (the Tailscale app shares none with other apps), so it could only add a chat peer by name or address. A chat peer that can list them, the Linux app or the device, now shares them with a contact that asks, and the Android app looks for chat peers among what its contacts share. Asked for by the user after the first phone run.

**Blocked by:** none (ticket 02's protocol, 05's core, 11's Android app)

**Status:** ready

### The protocol (version 1, a new capability; `docs/tailnet-chat/protocol.md`)

- A capability `peers`, used only when both hellos announce it (section 5).
- `{"type":"peers_ask"}`, answered with `{"type":"peers","peers":[{"name":"thistle-46cf20","addrs":["100.110.81.38","fd7a:115c:a1e0::2e38:5128"]}, ...]}`: the tailnet peers the answering side's platform shows online, less the asker. `name` is the tailnet hostname, at most 64 bytes; `addrs` 1 or 2 addresses inside the tailnet ranges (section 1). At most 64 peers. A peer with no list (Android) answers an empty one.
- Answered only to a **contact** (section 7). A stranger's `peers_ask` is answered with an empty `peers`; a peer that did not announce `peers`, or anything malformed, draws the usual `error` (section 4). An unasked `peers` is ignored.
- The asker MUST NOT take a key, a name or a trust decision from the list: it only learns addresses to probe with a hello on its discovery port, as Linux discovery does (spec, "Discovery"), and drops any address outside the tailnet ranges.
- `vectors/messages.json` gains valid and invalid frames; the reference peer (`tools/tailnet_chat/peer.py`) announces `peers`, answers from a list it is given (a command-line option and a control command), and can ask; `conformance.py` gains scenarios (answered to a contact, empty to a stranger, unannounced is unsupported, the limits), run against the reference peer both ways (`tools/test_tailnet_chat.py`), against the headless core and against the simulator.

### The device (`components/kernel_rs/src/tailnet_chat*.rs`)

- [ ] Announces `peers` and answers a contact's `peers_ask` from the tailnet's online peers the kernel keeps (`tailnet_manager.rs`), less the asker's own address. The rules are host-tested (`tailnet_chat_wire.rs`, `tailnet_chat_policy.rs`).

### The companion (`companion/core/`, `companion/app/`)

- [ ] The core announces `peers`, answers from a list the platform gives it (Linux: `tailscale status --json`'s online peers, as discovery reads them; Android: none), and has `askPeers(contact)`.
- [ ] On Android the Contacts page gets **Look for chat peers**: it asks each contact that announces `peers` (dialling it if needed, a few seconds each), merges what they share, probes each address on the discovery port with a hello, and lists the chat peers found, as Linux does, each with the contact it came through ("via laptop"). Contacts and this device are left out. Add works as on Linux.
- [ ] Dart tests for the core (`dart test`) and the app (`flutter test`); `flutter analyze` clean.

### Docs

- [ ] `protocol.md` section 4 and 5, `docs/tailnet-chat/README.md` if it lists capabilities, the companion's README, CLAUDE.md's Tailnet chat lines.

## Comments
