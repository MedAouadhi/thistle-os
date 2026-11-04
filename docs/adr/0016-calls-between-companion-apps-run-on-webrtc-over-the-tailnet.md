# Calls between companion apps run on WebRTC, over the tailnet only

Tailnet chat's calls are `ws-opus`: Opus frames on the chat WebSocket, which the T-Deck can run and the companion app does by hand, PulseAudio and Android's AudioRecord by FFI. Video between companion apps on Linux and Android needs camera capture, a video codec, rendering, congestion control and echo cancellation on both platforms, which the T-Deck has none of and will not get.

**A video-capable chat peer announces `call:webrtc`**, and every call between two that do runs on WebRTC, through flutter_webrtc and its prebuilt libwebrtc, camera on or not: video is a switch in any call, not another kind of call, so the call must start on a transport that can carry it. The SDP and ICE candidates travel as `call_rtc` frames on the authenticated chat connection, negotiated once with one audio and one video track, so a camera switch replaces a track and never renegotiates. A call with the T-Deck, or with a companion app from before, stays `ws-opus`; protocol v1 needs no new version.

**Media goes over the tailnet alone.** No STUN or TURN; each side sends and accepts only candidates in 100.64.0.0/10 and fd7a:115c:a1e0::/48, so a call is admitted and encrypted by WireGuard as every chat frame is (ADR-0015). WebRTC's own DTLS-SRTP runs inside it and is not relied on.

**VP8 is preferred and H.264 never.** The prebuilt Linux libwebrtc has OpenH264 (BSD-2) and Chromium's FFmpeg (LGPL-2.1) built in; Android's has no software H.264. No GPL. The binary is fetched at a pinned release and checked against a committed SHA-256. The Android artifacts, flutter_webrtc's libwebrtc and audio switch with everything else the Android build fetches, are pinned by Gradle's dependency verification (`companion/app/android/gradle/verification-metadata.xml`); another hash fails the build.

## Considered Options

**A `ws-video` transport of our own**, capture, VP8 and rendering by FFI on both platforms, over the chat WebSocket like `ws-opus`, adds no dependency, but is weeks of media code, and over TCP, through DERP above all, any loss stalls the picture.

**A separate video call on WebRTC, voice calls left on `ws-opus`**, would leave voice untouched, but a call could never gain video without hanging up.

**Upgrading a `ws-opus` call to WebRTC when a camera comes on** keeps voice as it is until then, at the cost of a live handover between two media paths mid-call.

**medea_flutter_webrtc** is archived, bundles OpenAL Soft (LGPL) and is x86_64 only.

**A libwebrtc built without H.264 and FFmpeg** would make the licences clean, at the cost of building Chromium's WebRTC and keeping the binary ourselves.

**LAN candidates beside the tailnet's** would shorten the path on one WiFi, but media would leave the tailnet and rest on DTLS alone.

## Consequences

The companion app has two media paths, `ws-opus` for the T-Deck and WebRTC between companion apps, and its voice calls with another companion app move to WebRTC: flutter_webrtc's Linux bugs (a UI freeze of about 10 s on each call after the first, issue 2070; a camera kept busy by child processes, issue 2225) reach voice calls too, which is why a spike comes first. Since the offer lists both transports, a build can stop offering `webrtc` and calls fall back to `ws-opus` with no protocol change. A WebRTC call on Linux uses libwebrtc's echo canceller, not PipeWire's. The Linux binary is about 25 MB and Android's adds about 12 MB to each split APK, and the Linux one ships patent-encumbered H.264 code that is never negotiated. With no relay of its own, a call goes through DERP when Tailscale has no direct path, and an Android app outside the Tailscale app's split tunnel cannot call at all. libwebrtc on Android gathers no candidate on the Tailscale app's interface, and flutter_webrtc exposes nothing that changes it, so the phone signals no candidate and a call connects from the other side's: the phone's checks reach the other side's tailnet address through the VPN, a peer-reflexive pair with tailnet addresses at both ends. Two phones would have no candidate between them, so the app on Android also announces `call:webrtc-prflx`, and a call between two that both announce it runs on `ws-opus`, voice only; a phone and a laptop stay on WebRTC. On Android the camera pauses while the app is away (`call_camera` `paused`) and the voice goes on in the listening service, which has no camera type; there is no picture-in-picture.
