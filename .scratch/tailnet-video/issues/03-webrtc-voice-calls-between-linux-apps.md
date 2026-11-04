# 03: Calls between Linux companion apps run on WebRTC

**What to build:** two Linux companion apps announce `call:webrtc`, and a call between them runs on WebRTC, voice both ways, with libwebrtc's echo canceller (no PipeWire echo canceller started) and the call screen's echo line saying so. A call between a Linux app and the T-Deck, or an older companion app, is still `ws-opus`, as today. This cuts through every layer: the protocol document's new section and vectors (`call:webrtc`, `webrtc` first in `call_offer` with `video`, `call_rtc`, `call_camera`), the reference chat peer, the core's policy and frames (SDP and candidates opaque), the app's call media engine seam with a flutter_webrtc implementation and a fake, and the build's pinned libwebrtc. See `../spec.md`, "The protocol (still version 1)", "The companion core", "The companion app"; ADR-0016.

**Blocked by:** 02

**Status:** ready-for-agent

- [ ] The protocol document and its test vectors describe `call:webrtc`, the offer's transports and `video`, `call_rtc` (SDP once, then trickled candidates; one audio and one video track negotiated from the start), `call_camera` (on, off, paused, none), tailnet-only candidates, and the 15 s connect and 10 s disconnect ends
- [ ] The core's policy, host-tested: the offer's transport list and the answer's pick from both hellos, candidate admission by the tailnet's ranges, the connect and disconnect timeouts
- [ ] The reference chat peer speaks the new frames; the conformance suite against the core covers the transports with and without the capability on either side, the pick, SDP and candidates relayed, a non-tailnet candidate neither sent nor taken, `call_camera`, a call never connected and one disconnected for 10 s, and busy during a WebRTC call
- [ ] The suite against the simulator shows the T-Deck announces no `call:webrtc` and answers an offer of both with `ws-opus`
- [ ] A call media engine interface beside the audio devices, with a flutter_webrtc implementation (no ICE servers; VP8 then Opus preferred) and a fake; the app's call tests drive a WebRTC voice call with the fake
- [ ] The Linux build fetches libwebrtc at a pinned release and checks a committed SHA-256 before Flutter configures, locally and in CI; the third-party licences list flutter_webrtc and libwebrtc's contents
- [ ] By hand, written here: a call between two Linux machines on the tailnet, heard both ways; a call from each to the T-Deck still `ws-opus`; the log says the transport, the candidates' kinds and why the call ended
