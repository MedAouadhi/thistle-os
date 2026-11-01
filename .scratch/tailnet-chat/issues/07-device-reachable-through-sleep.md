# 07: Device: reachable through sleep

**What to build:** a T-Deck that sleeps still gets its messages. The module registers a background check, "tailnet chat", every `thistle_os.tailnet_chat.check_minutes` (15; 0 is off), settable in the app. That check, and every rejoin after a sleep or a change of network, dials each contact with something in the outbox and each contact the tailnet peer list shows online, so both sides' outboxes flush. A flush holds the device awake.

**Blocked by:** 01, 04

**Status:** ready-for-agent

- [ ] Which contacts to dial on a check or a rejoin is a host-tested policy.
- [ ] The check logs what it did as other background checks do, and is skipped, not forced, when internal RAM is short.
- [ ] A flush holds the device awake under its name and releases it when done or failed.
- [ ] The simulator's test registers the check with a short interval and shows a message from the reference peer arriving through it.
- [ ] On hardware: a message sent from the Linux app to a light-sleeping T-Deck arrives within one interval, and one sent from the T-Deck while the laptop's app was closed arrives when it is opened; written in the comments.
