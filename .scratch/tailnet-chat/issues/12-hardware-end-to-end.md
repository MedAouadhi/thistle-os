# 12: On hardware, end to end

**What to build:** the whole feature proven on a T-Deck Max with the Linux and Android apps, and written down for the next person. Results go in this ticket's comments; what a developer needs goes into CLAUDE.md's section on Tailnet chat.

**Blocked by:** 07, 08, 09, 10, 11

**Status:** ready-for-human

- [ ] A call from the phone to the T-Deck and back over WiFi, and over LTE through DERP: delay, frames lost, and how the echo sounds, written down.
- [ ] Free internal RAM at boot with the tailnet and Tailnet chat on, and with a call up; the call does not starve another task.
- [ ] A day of use: messages to a sleeping T-Deck collected by the background check, voice messages both ways, the Android app in the background, a contact's app reinstalled and its new key accepted.
- [ ] An SMS during a call over LTE arrives after it; a cellular call during a tailnet call ends it.
- [ ] CLAUDE.md has a Tailnet chat section in the style of the others: the module, the app, the protocol document, the companion app, the simulator's switches and tests.

## Comments

- 2026-11-02, the first bench run on the T-Deck Max (WiFi, no SIM): the laptop reached the device, but the device never reached the laptop, though the tailnet's policy allowed it. The device was homed in DERP region par, the laptop in nue, and MicroLink sent everything through its own region's relay, which delivers only to clients connected to it. MicroLink 2a13717 sends to a peer through the peer's home region (bcb5f34); the device's dial to the laptop then connected. It also takes MicroLink 1dd7a6c, a peer's online as the control plane says it (98ef6e0). The policy needs `tag:hermes-device` allowed to `autogroup:member:4810`.
