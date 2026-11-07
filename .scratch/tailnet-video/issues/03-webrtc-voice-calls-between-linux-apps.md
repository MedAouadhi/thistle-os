# 03: Calls between Linux companion apps run on WebRTC

**What to build:** two Linux companion apps announce `call:webrtc`, and a call between them runs on WebRTC, voice both ways, with libwebrtc's echo canceller (no PipeWire echo canceller started) and the call screen's echo line saying so. A call between a Linux app and the T-Deck, or an older companion app, is still `ws-opus`, as today. This cuts through every layer: the protocol document's new section and vectors (`call:webrtc`, `webrtc` first in `call_offer` with `video`, `call_rtc`, `call_camera`), the reference chat peer, the core's policy and frames (SDP and candidates opaque), the app's call media engine seam with a flutter_webrtc implementation and a fake, and the build's pinned libwebrtc. See `../spec.md`, "The protocol (still version 1)", "The companion core", "The companion app"; ADR-0016.

**Blocked by:** 02

**Status:** in-review (the by-hand run between two machines and to the T-Deck is the user's)

- [x] The protocol document and its test vectors describe `call:webrtc`, the offer's transports and `video`, `call_rtc` (SDP once, then trickled candidates; one audio and one video track negotiated from the start), `call_camera` (on, off, paused, none), tailnet-only candidates, and the 15 s connect and 10 s disconnect ends
- [x] The core's policy, host-tested: the offer's transport list and the answer's pick from both hellos, candidate admission by the tailnet's ranges, the connect and disconnect timeouts
- [x] The reference chat peer speaks the new frames; the conformance suite against the core covers the transports with and without the capability on either side, the pick, SDP and candidates relayed, a non-tailnet candidate neither sent nor taken, `call_camera`, a call never connected and one disconnected for 10 s, and busy during a WebRTC call
- [x] The suite against the simulator shows the T-Deck announces no `call:webrtc` and answers an offer of both with `ws-opus`
- [x] A call media engine interface beside the audio devices, with a flutter_webrtc implementation (no ICE servers; VP8 then Opus preferred) and a fake; the app's call tests drive a WebRTC voice call with the fake
- [x] The Linux build fetches libwebrtc at a pinned release and checks a committed SHA-256 before Flutter configures, locally and in CI; the third-party licences list flutter_webrtc and libwebrtc's contents
- [ ] By hand, written here: a call between two Linux machines on the tailnet, heard both ways; a call from each to the T-Deck still `ws-opus`; the log says the transport, the candidates' kinds and why the call ended

## Comments

**2026-11-04, implementer.** Branch `tv-03-webrtc-linux`, from
`tailnet-video` at 94b27c6. Every box but the last is done and tested; the
last needs a second machine and the T-Deck, neither of which the agent had.

What was built:

- **Protocol** (`docs/tailnet-chat/protocol.md` 4.8, 4.11, 5):
  `call:webrtc`; the offer lists `webrtc` first only when both hellos
  announce it, with `video`; the answer picks `webrtc` whenever offered and
  announced; `call_rtc` (SDP once, then trickled candidates) and
  `call_camera` (on, off, paused, none); tailnet-only candidates, in a
  frame or an SDP's `a=candidate` lines; the 15 s `timeout` and 10 s
  `error` ends; a WebRTC call is never ended for want of `ws-opus` frames.
  `vectors/webrtc.json` has one of each frame, frames to refuse, nine
  candidates with whether they are admitted, and an SDP before and after
  filtering. It is a file of its own, not part of `messages.json`, because
  the kernel's tests take every frame of `messages.json` as one the device
  must check, and the device knows none of these.
- **Deviation:** candidate admission is UDP only, as well as the
  tailnet's ranges (protocol 4.11 says so, and SHOULD gather no TCP
  candidates), from ticket 02's TCP finding. The spec said addresses only.
- **Core**: `offerTransports`, `pickTransport`, `candidateAdmitted`,
  `tailnetOnlySdp`, `candidateKind` and the WebRTC ends in `callEndDue`
  (policy, host-tested); `ChatPeer.call(video:)`, `sendRtc`, `setCamera`,
  `setMediaState`, `callRtc`, and `Call.transport`/`video`/`remoteCamera`/
  `media`. The headless core takes `--webrtc` and the control commands
  `set webrtc`, `rtc`, `rtc_received`, `camera`, `media`.
- **Reference peer and suite**: eight `webrtc_*` scenarios. Against the
  headless core: 37 of 38 pass (`call_ended_by_cellular` skipped, no
  modem), the WebRTC ones included. Against the reference peer: 38 of 38.
  Against the simulator (`test_tailchat.py --only calls`, built here):
  the call scenarios and `webrtc_unannounced_answers_ws_opus` pass, so the
  T-Deck announces no `call:webrtc` and answers an offer of both with
  `ws-opus`.
- **App**: `CallEngine`/`CallSession` (`lib/src/call_engine.dart`),
  `WebrtcCallEngine` over flutter_webrtc 1.6.3 (`lib/src/webrtc_engine.dart`:
  no ICE servers, `max-bundle`, `rtcp-mux` required, TCP candidates
  disabled, an audio track and a sendrecv video transceiver from the
  start, VP8 first and no H.264, Opus first; a frame drawn, or 250 ms,
  before getUserMedia), and `FakeCallEngine` in the tests. Talk starts the
  engine for a `webrtc` call and the old devices for a `ws-opus` one;
  sends `call_camera off` at the answer; reports the connection's state to
  the core. Linux announces `call:webrtc` unless `--no-webrtc`; Android
  does not yet (ticket 05). The call screen says "Call ended: it could
  not connect" for a WebRTC call that never connected.
- **The pair check, changed from ticket 02's advice.** Read at the moment
  ICE first connects, the selected pair was a peer-reflexive LAN address
  (192.168.2.131) to the tailnet's 100.91.111.54 in every run on this
  machine, even with `max-bundle`; within 0.5 s it moved to the tailnet's
  IPv6 pair at both ends. Checked then, every call ended as an error. The
  app now reads the pair 2 s after connecting and every 5 s after, and
  ends the call only if it is outside the tailnet then. On one machine
  that first half second of LAN-to-tailnet is local delivery; between two
  machines a packet from a LAN socket to a tailnet address should not get
  through WireGuard, but the hardware run should look at the first `media
  over` line and at `tcpdump -i <lan>` for any UDP to the other machine.
- **Build**: `companion/app/linux/libwebrtc.cmake`, included before the
  plugins, fetches `libwebrtc.m150.7871.07` and checks SHA-256
  `ef380526...89fbc` (the same hash downloaded twice, once by the spike
  and once here), replaces a zip with another hash, and unpacks it with a
  stamp naming the hash; a wrong pin fails the build (tried). It also
  fails if flutter_webrtc asks for another release. linux-x64 only. The
  bundle is 55.0 MB.
- **Licences**: flutter_webrtc and its transitive packages (MIT), and
  libwebrtc's contents as found in the binary's strings: WebRTC, libvpx,
  Opus and the rest BSD-3, BoringSSL, Abseil, dav1d and OpenH264 BSD-2,
  and Chromium's FFmpeg, LGPL-2.1.

Verified here: core `dart test` (all), app `flutter test` (110),
`flutter analyze --fatal-infos`, `dart format`, the suite against the
headless core, the reference peer and the simulator, `flutter build linux
--release`, `flutter build apk --release`, and Python's
`tools/test_tailnet_chat.py`.

**In place of two machines**: `companion/app/tool/webrtc_call_check.dart`
runs two of the app's models in one process with the real libwebrtc,
each listening on 100.91.111.54, under Xvfb with this laptop's PulseAudio
(release build, PASS): two calls in a row, each connected 140 to 470 ms
after the offer across runs, over
`host/udp fd7a:115c:a1e0::3936:6f36` at both ends after the first half
second, only tailnet candidates sent (`host/udp other candidate not
sent` for every other), the camera said off both ways, hung up cleanly.
It shows signalling and connection, not sound: no one listened, and both
ends were one machine. Running two app windows was not possible without a
display.

**Left for the user, by hand:**

1. Two Linux machines on the tailnet, both on this branch's build: call
   each way, talk, and check each hears the other with no echo from the
   laptop's own speaker; the call screen's echo line says WebRTC's
   canceller. With `--verbose`, the log has `call ... over webrtc`, each
   candidate's kind with sent or not sent, `media over host/udp
   <tailnet> to host/udp <tailnet>`, and `ended:hangup, over webrtc`.
2. From each machine, a call to the T-Deck: still `ws-opus` (the log says
   `over ws-opus`, the echo line PipeWire's canceller), as before.
3. A second call in a row on each machine, for the freeze (2070).
