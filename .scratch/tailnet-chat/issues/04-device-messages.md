# 04: Device: messages, the outbox, alerts, receipts, typing, replies and reactions

**What to build:** chatting from the device. The user writes in a conversation and sends; a message to a contact who is away waits in the outbox and is sent when the contact connects or on the backoff, until acked; a duplicate is never shown twice. A received message buzzes, pops up and counts in the status bar's envelope and Home's new-messages row like an SMS, unless its conversation is on screen. Delivered and read receipts, typing, replies and reactions work both ways, and the user can switch off sending read receipts. History is read back after a restart. Arabic and dictation work in the compose field as in any text field.

**Blocked by:** 03

**Status:** ready-for-agent

- [ ] The outbox's retries are a host-tested policy: on connect, on a peer seen online, and on a backoff of 30 s doubling to 30 minutes; nothing expires; typing and read receipts are not queued.
- [ ] The outbox and history survive a restart; messages are shown in the order written.
- [ ] Message alerts have a third kind, under `thistle_os.messenger.alerts`, suppressed for the conversation on screen.
- [ ] `thistle_os.tailnet_chat.read_receipts` (on) is in the app's settings and respected.
- [ ] A reply shows what it quotes; a reaction can be changed and removed; typing lapses after 6 s.
- [ ] A conversation shows its latest 20 messages with shared styles; LVGL's pool is measured with a full conversation open.
- [ ] The conformance suite's message, outbox, duplicate, receipt, typing, reply and reaction scenarios pass against the simulator, which also checks the history files, the status bar's count and Home's row.
