# 04: Device: messages, the outbox, alerts, receipts, typing, replies and reactions

**What to build:** chatting from the device. The user writes in a conversation and sends; a message to a contact who is away waits in the outbox and is sent when the contact connects or on the backoff, until acked; a duplicate is never shown twice. A received message buzzes, pops up and counts in the status bar's envelope and Home's new-messages row like an SMS, unless its conversation is on screen. Delivered and read receipts, typing, replies and reactions work both ways, and the user can switch off sending read receipts. History is read back after a restart. Arabic and dictation work in the compose field as in any text field.

**Blocked by:** 03

**Status:** done in the simulator; not yet seen on hardware

- [x] The outbox's retries are a host-tested policy: on connect, on a peer seen online, and on a backoff of 30 s doubling to 30 minutes; nothing expires; typing and read receipts are not queued.
- [x] The outbox and history survive a restart; messages are shown in the order written.
- [x] Message alerts have a third kind, under `thistle_os.messenger.alerts`, suppressed for the conversation on screen.
- [x] `thistle_os.tailnet_chat.read_receipts` (on) is in the app's settings and respected.
- [x] A reply shows what it quotes; a reaction can be changed and removed; typing lapses after 6 s.
- [x] A conversation shows its latest 20 messages with shared styles; LVGL's pool is measured with a full conversation open.
- [x] The conformance suite's message, outbox, duplicate, receipt, typing, reply and reaction scenarios pass against the simulator, which also checks the history files, the status bar's count and Home's row.

## Comments

Done on branch `tailnet-chat-04-device-messages`.

- **The outbox's policy** is `tailnet_chat_policy.rs`: `Retry` (`due`, `failed` on a dial that never opened or a connection that closed with something waiting, `seen_online`, `connected`), `retry_delay_ms` (30 s doubling to 30 min, no expiry), `is_queued`, `to_dial` (contacts with something waiting, not connected, with an address, due; at most the connections left of 4, dials in flight counted), `keep_new_connection` (the lower key's dial wins), `is_typing` (6 s), `reaction_replaces`. All host-tested. Retry state is in memory: after a restart everything waiting is tried at once. Triggers now: a new message or reaction, the tailnet coming back (the listener restarting), `flush()` (the control port's). Ticket 07 calls `flush()` from the peer list and the background check.
- **Dialling.** The module dials at last: the blocking TCP connect runs on a one-shot `kernel_task` (`chat_dial`, 6 KB internal stack, 5 s timeout) and hands the socket back to the poll job, which upgrades (masked frames, `wire::upgrade_request`/`upgraded`) and runs the dialler's half of the handshake. A name resolves through the tailnet's map only (`tailnet_manager::resolve_peer`), never DNS.
- **Contacts added by address** (`tailnet_chat_add_contact`, the control port's `add_contact`): a placeholder key until the first dialled handshake pins the real one; the outbox and the history file follow it (`msg_log::chat_rekey`). Needed by the conformance suite's outbox scenarios. No screen for it yet.
- **History.** The card file stays append-only: a state change (`{"dir":"out","id":X,"state":"delivered"}`) and a reaction (`{"react":X,"emoji":"👍",...}`) are lines of their own after the message, folded together when a conversation is read back (its last 200 lines). Those 200 lines' ids are also the duplicate window, so a message or reaction sent again is never shown twice, after a restart too (story 23), as long as history is kept.
- **Alerts and counts.** `msg_alert` has `MSG_ALERT_TAILNET` (2), the conversation being the key's fingerprint id; a contact request alerts too. Unread counts are per contact in the module's file; the status bar's envelope and Home's row add `tailnet_chat_unread_count()`, and open Chat when only Chat has unread messages. The conversation on screen (`tailnet_chat_set_viewing`) is read as messages arrive: no alert, and the read receipt goes after the ack.
- **Capabilities.** The hello announces `text, receipts, typing, reply, react`; `read` and `typing` are sent only to a peer that announced them too.
- **The app.** Compose field (one line; Enter or Send), a message chosen offers Reply and four reactions (or Remove), bubbles show what a reply quotes, Waiting/Delivered/Read and reactions; typing is said every 4 s while keys come and unsaid 5 s after; "Read receipts" switch on the Contacts tab; the request page shows every held message. Emoji are written as text ("+1", "<3"), and "..." replaces "…", which Montserrat lacks. **LVGL's pool with a full conversation open (20 messages, the longest cut at 160 characters, Arabic, replies, reactions): 32 KB free, largest block 32 KB.**
- **Beyond the spec:** the simulator test is renamed `simulator/tests/test_tailchat.py`, as the thistle-app skill's wiring check looks for `test_<app>*.py`; the control port fflushes stdout after each command so tests see the UI's log lines.
- **Simulator.** All conformance scenarios but the voice message's pass (15). New cases: `messages` (pop-up, buzz, the status bar's count, Home's row, reply, receipts and reactions on the card, the conversation on screen, the outbox and history across a restart, a duplicate after a restart, a contact added by address pinned when it comes back) and `app-conversation` (the pool, read on opening, a message typed and sent). The Dart core passes the whole suite (16/16) unchanged.
- **Not seen on hardware:** dialling over WireGuard (the first SYN is dropped while the session starts), the dial task's stack, free internal RAM with chat on.
