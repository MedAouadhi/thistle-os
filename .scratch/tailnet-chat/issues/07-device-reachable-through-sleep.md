# 07: Device: reachable through sleep

**What to build:** a T-Deck that sleeps still gets its messages. The module registers a background check, "tailnet chat", every `thistle_os.tailnet_chat.check_minutes` (15; 0 is off), settable in the app. That check, and every rejoin after a sleep or a change of network, dials each contact with something in the outbox and each contact the tailnet peer list shows online, so both sides' outboxes flush. A flush holds the device awake.

**Blocked by:** 01, 04

**Status:** done in the simulator; on hardware the wake, the check and the sweep run, but a message to the sleeping T-Deck is blocked by the tailnet's policy (see Comments)

- [x] Which contacts to dial on a check or a rejoin is a host-tested policy.
- [x] The check logs what it did as other background checks do, and is skipped, not forced, when internal RAM is short.
- [x] A flush holds the device awake under its name and releases it when done or failed.
- [x] The simulator's test registers the check with a short interval and shows a message from the reference peer arriving through it.
- [ ] On hardware: a message sent from the Linux app to a light-sleeping T-Deck arrives within one interval, and one sent from the T-Deck while the laptop's app was closed arrives when it is opened; written in the comments.

## Comments

Done on branch `tailnet-chat-07-sleep`.

- **Who is dialled** is `tailnet_chat_policy.rs`, host-tested: `to_sweep` (each contact with something waiting and each the peer list shows online, not connected, with an address), `to_dial` (those a sweep asked for first, whatever the backoff, then those waiting whose retry is due, as the 4 connections allow), `came_online`, `sweep_settled` (a connection a sweep opened closes once nothing has come for 3 s and nothing of ours waits for an ack), `hold_awake` (2 minutes at most).
- **Sweeps** run on every join of the tailnet (`tailnet_manager::joins()`, counted each time the client connects: after a sleep, a change of network, a restart), on a peer seen coming online (only its contact), from the background check, and for a contact added while its peer is online, so its key is pinned at once. A contact is matched to the peer list by the address it was last seen at, or the address or tailnet name it was added by. **MicroLink at the pinned commit reports every peer online**, so "seen coming online" never fires and every join sweeps every contact with an address, until `.scratch/tailnet-chat/microlink-peer-online.patch` lands on the fork.
- **The check** "tailnet chat" is registered at `check_minutes` (15; 0 off; 24 KB of internal RAM, so it is skipped when RAM is short; 4 KB stack). It waits up to 60 s for the tailnet, sweeps, waits up to 45 s for the sweep to settle, and logs `check tailnet chat: done (dialled 1, 1 received, 0 delivered) in 5.2 s; ...`. A sweep the wake's own join already started counts as the check's.
- **The hold** is `tailnet chat`, taken while a dial is in flight, a sweep has contacts left to dial or a swept connection open, or an open connection has our frames waiting for an ack; 2 minutes at most, so a peer that never acks cannot keep the device from sleeping. Logs `holding the device awake: delivering` and `deliveries over`.
- **The app**: Contacts has `Add contact` (a tailnet name or address, `:port` if not 4810, and a name; the parsing is host-tested in `tailchat_core.c`) and `Check while asleep: 15 min`, which goes round 5, 15, 30 minutes, 1 h and off; saved as `check_minutes`. C API `tailnet_chat_check_minutes()` / `tailnet_chat_set_check_minutes()`.
- **Simulator**: `THISTLE_SIM_TAILNET_LOOPBACK_PEER=NAME[:offline]` puts a fake tailnet peer at 127.0.0.1, `THISTLE_SIM_CHAT_CHECK_S` sets the check's interval in seconds. `test_tailchat.py` cases `sleep-check` (a reference peer that has no address for the device writes to it; the check, every 12 s, dials it and the message arrives; the add sweep, the hold and the swept connection's close are in the log) and `app-add-contact`.

### On hardware (T-Deck Max, 2026-11-03)

Driven from the laptop with a temporary bench hook, never committed: it added the laptop as a contact, sent one message, asked for light sleep, and checked every 120 s.

- **The wake works.** Asleep at 01:53:38. The check's timer woke it at 01:55:05 (`up for a background check`). WiFi was back at 01:55:09 and the tailnet at 01:55:12, when the join swept and dialled the contact. The check was done at 01:55:17 (`background checks over; sleeping again`), about 12 s awake. The same at every interval after: 01:57:17, 01:59:30, 02:01:44, 02:04:07. Internal RAM 123 KB free before a check and 99 KB after, with the tailnet up. The check's 4 KB stack had 2116 bytes never used. Before this fix, the check said `nobody to dial` when the wake's join had already dialled.
- **The T-Deck cannot reach the laptop: the tailnet's policy drops it.** Every dial from the T-Deck to the laptop (100.91.111.54:4810) timed out after 5 s, awake or woken, while the laptop reached the T-Deck's port at once. The laptop's packet filter (`tailscale debug netmap`) admits only the user's own devices and homelab. The T-Deck is a tagged node (`tagged-devices`, as Hermes needs), so nothing it opens reaches the laptop. Neither half of the box can pass until the policy lets the T-Deck's tag reach the user's devices on 4810, for example `{"action": "accept", "src": ["tag:hermes-device"], "dst": ["autogroup:member:4810"]}` (adjust the tag). Laptop to T-Deck works today, so a message reaches the T-Deck while it is awake. While it sleeps, only the T-Deck can collect what waits for it.
- **Remaining manual**, once the policy allows it: a message from the Linux app to a light-sleeping T-Deck arriving within one interval, and one sent from the T-Deck while the laptop's app was closed arriving when it opens. Also the free internal RAM with a chat delivery under way, and the 24 KB the check asks for.

