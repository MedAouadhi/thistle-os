# 02: Spike: a WebRTC call over the tailnet, Linux to Android

**What to build:** a throwaway prototype that answers what the rest of the plan rests on, with flutter_webrtc and its prebuilt libwebrtc (ADR-0016): does a VP8 call with only tailnet candidates (100.64.0.0/10, fd7a:115c:a1e0::/48), no STUN or TURN, connect between the Linux app and the Android app, both ways, directly and through DERP; does flutter_webrtc issue 2070 (the UI frozen for about 10 s on each call after the first, on Linux) happen in this app; does issue 2225 (the camera kept busy by a child process, such as `tailscale status`) happen; how much the APK and the Linux bundle grow. Signalling may be hand-wired; nothing of it needs to be kept. See `../spec.md`, "Order of work".

**Blocked by:** None (can start immediately)

**Status:** ready-for-agent

- [ ] A Linux to Android call and an Android to Linux call, each with audio and video both ways, over tailnet candidates only, on WiFi and through DERP; the candidates' kinds logged
- [ ] Issue 2070: three calls in a row on Linux, the time each set-up and hang-up blocks the UI, written here
- [ ] Issue 2225: the camera opened while the app runs `tailscale status`, and whether a second call can open it, written here
- [ ] The release APK's split sizes and the Linux bundle's size, before and after
- [ ] A verdict in this ticket: go, or stop for a decision (if 2070 is confirmed, the plan stops here, as the spec says)
