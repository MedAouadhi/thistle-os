# 10: Companion: voice messages and live calls

**What to build:** the companion app records, sends, receives and plays voice messages, and places and takes live `ws-opus` calls, with the platform's echo cancellation on, on Linux. It does both with the simulator, with a T-Deck, and with another companion app.

**Blocked by:** 06

**Status:** ready-for-agent

- [ ] Voice messages are Ogg/Opus at 16 kHz mono, up to 2 minutes, as the protocol's vector.
- [ ] Calls ring with a notification when the app is in the background; answer, decline, hang up and mute work.
- [ ] The conformance suite's voice and call scenarios pass against the headless core.
- [ ] A manual run, written in the comments: a voice message and a call each way with the simulator, and a call between two Linux instances.
