# 09: Device: live calls

**What to build:** 1:1 voice calls between the device and a contact. The user calls from a conversation or a contact; an incoming call rings on the system call screen, extended to show a tailnet contact, over whatever app is in front; answer, decline, hang up, mute and volume work. Audio is `ws-opus` with a jitter buffer and Opus' loss concealment, and the microphone is turned down while the far end is loud. One call at a time: a second gets busy. A cellular call that rings ends a tailnet call; an SMS that arrives during a tailnet call over LTE is fetched after it. A call to an unreachable contact fails at once, and a missed one goes into the call log.

**Blocked by:** 01, 04

**Status:** ready-for-agent

- [ ] The call rules are a host-tested policy: one at a time, busy, cellular wins, SMS deferred, 10 s without a frame ends the call.
- [ ] A call runs on a task made for it and gone after, with an internal stack sized from ticket 01; it holds the device awake, takes the audio, sets the codec to 16 kHz mono, and restores the player's format after.
- [ ] The call screen shows a tailnet contact's name and the call's state, from the LVGL window managers.
- [ ] The log says each call's start, transport, end and reason, and frames lost.
- [ ] The conformance suite's call scenarios (answered, declined, busy, ended by a cellular call, the far end gone silent) pass against the simulator, using the fake modem's `--call` for the cellular case; the test checks the call screen by screenshot and that the simulator plays what the reference peer sends.
