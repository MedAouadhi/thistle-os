# 06: Replies, reactions, typing and read receipts

**What to build:** In a chat, the user can reply to a particular message
(shown quoted above the reply, in and out) and react to a message
(reactions shown on the message they belong to). The other side's
"typing…" shows while they type, and the user's own typing is sent.
Opening a chat sends read receipts for what it shows, so the phone and
the senders agree on what has been read.

**Blocked by:** 04

**Status:** ready-for-agent

- [ ] `test_whatsapp.py`: a reply to a message reaches the fake with its quoted id; an incoming reply shows its quote; a reaction goes out and an incoming one shows on its message; the fake's typing shows and stops; typing in the field sends typing; opening a chat sends read receipts for its unread messages, once
- [ ] Reactions and quotes are kept in the `msg_log` lines so they survive a restart
- [ ] On the device, with the spare number: each of the above seen on the phone

## Comments
