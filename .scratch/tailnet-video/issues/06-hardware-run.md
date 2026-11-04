# 06: The manual hardware run

**What to build:** the spec's manual run, done on a Linux laptop, a second Linux machine, an Android phone and the T-Deck, and written down here, so that video calls and the locked-phone ring are known to work on real hardware. See `../spec.md`, "Testing Decisions", Hardware.

**Blocked by:** 05

**Status:** ready-for-agent

- [ ] Linux to Android and back, and Linux to Linux, over WiFi directly and through DERP, and with mobile data on the phone: video both ways, the camera switched mid-call
- [ ] A laptop without a camera; camera permission refused on the phone
- [ ] Leaving the app on Android during a video call, and coming back
- [ ] The loudspeaker with video and the earpiece without; echo with the laptop's own speaker and microphone
- [ ] A call to the T-Deck from the Linux app and from the phone, unchanged (`ws-opus`)
- [ ] Three calls in a row on Linux, for the freeze (flutter_webrtc issue 2070)
- [ ] The selected-pair check between two machines. On one machine (tickets 02 and 03) the pair selected as ICE first connects is a peer-reflexive LAN address to the other side's tailnet address, which moves to tailnet addresses at both ends within half a second; the app reads it 2 s after connecting and every 5 s after, and ends the call as `error` if either end is outside the tailnet (protocol section 4.11). Open: whether, between two machines, that first LAN-to-tailnet pair appears at all, and whether it carries anything. Read the first `media over` line of each call on both sides, run `tcpdump -i <lan interface> udp and host <the other machine's LAN address>` during the first seconds of a call, and check no call ends as "its media left the tailnet"
- [ ] On a locked phone: a video call from the Linux app rings with the ringtone and offers Answer with video; the ringer on vibrate and silent; Do Not Disturb
