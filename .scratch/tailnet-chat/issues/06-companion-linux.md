# 06: Companion app on Linux, talking to the simulator and to itself

**What to build:** the Flutter app on Linux over the core: Conversations, Contacts and Requests, the fingerprint view, settings (read receipts, background listening). It finds chat peers from `tailscale status --json` by probing each online tailnet peer's port, and takes a name or address by hand. Its identity key is kept in the Secret Service. Messages notify when it is in the background. It tells the user when Tailscale is not running. It chats with the simulator, and two instances on two machines of the tailnet chat with each other.

**Blocked by:** 05

**Status:** ready-for-agent

- [ ] Builds and runs on Linux from documented commands; the build is in CI.
- [ ] Discovery lists the chat peers among the online tailnet peers; a peer not discovered can be added by name or address.
- [ ] The identity key survives a restart and is not in a plain file.
- [ ] Every feature of tickets 03 and 04 is usable from the UI: requests, accept and decline, key change warning and acceptance, rename, remove, text, replies, reactions, typing, receipts.
- [ ] A manual run, written in the comments: the app chats with the simulator, and with a second instance on another tailnet machine.
