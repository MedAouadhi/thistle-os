# 02: Spike: a WebRTC call over the tailnet, Linux to Android

**What to build:** a throwaway prototype that answers what the rest of the plan rests on, with flutter_webrtc and its prebuilt libwebrtc (ADR-0016): does a VP8 call with only tailnet candidates (100.64.0.0/10, fd7a:115c:a1e0::/48), no STUN or TURN, connect between the Linux app and the Android app, both ways, directly and through DERP; does flutter_webrtc issue 2070 (the UI frozen for about 10 s on each call after the first, on Linux) happen in this app; does issue 2225 (the camera kept busy by a child process, such as `tailscale status`) happen; how much the APK and the Linux bundle grow. Signalling may be hand-wired; nothing of it needs to be kept. See `../spec.md`, "Order of work".

**Blocked by:** None (can start immediately)

**Status:** resolved

- [x] A Linux to Android call and an Android to Linux call, each with audio and video both ways, over tailnet candidates only, on WiFi and through DERP; the candidates' kinds logged
- [x] Issue 2070: three calls in a row on Linux, the time each set-up and hang-up blocks the UI, written here
- [x] Issue 2225: the camera opened while the app runs `tailscale status`, and whether a second call can open it, written here
- [x] The release APK's split sizes and the Linux bundle's size, before and after
- [x] A verdict in this ticket: go, or stop for a decision (if 2070 is confirmed, the plan stops here, as the spec says)

## Comments

**2026-11-04, implementer.** Branch `tv-02-spike`, from `tailnet-video`
at 52a0b56. Linux only: no phone was attached and there is no second
machine, so the Android calls and the DERP path are still to do (below).
The prototype is `companion/spike_webrtc/`, a separate Flutter project
(Linux and Android) on flutter_webrtc 1.6.3 (libwebrtc m150.7871.07,
published 2026-10-26). It is kept only so the phone run below can be
repeated; delete it when this ticket closes. Nothing in `companion/app`
changed.

Machine: Linux laptop, tailnet 100.91.111.54 /
fd7a:115c:a1e0::3936:6f36, Integrated_Webcam_HD (`/dev/video0`), Flutter
3.47.9. No display was available to the agent, so every run was under
**Xvfb** (`xvfb-run -s "-screen 0 1280x800x24"`, Mesa software GL).

**How the UI was measured.** The app runs a 10 ms `Timer.periodic` on the
UI isolate and logs any gap over 100 ms, tagged with the call in flight,
and every native call is timed around its `await`. On this Flutter the
platform and UI threads are one thread on Linux: `getUserMedia`, a
synchronous native call, stops the timer for exactly its own duration. A
call that blocked the platform thread would show up as a timer gap, as
2070's 10 s does. `SchedulerBinding.addTimingsCallback` also records gaps
between frames, with a `CircularProgressIndicator` animating throughout.

**The harness reproduces 2070 on an old version and does not on 1.6.3.**
I built the same app against flutter_webrtc 1.4.1, the version in the
issue. Call 1 was normal. Call 2: `A.setLocalDescription` 10 004 ms,
`B.setLocalDescription` 20 007 ms, `A.setRemoteDescription` about 10 s,
with matching timer gaps (10 008, 20 014, 10 008 ms). Set-up took
40.9 s, and video then hardly flowed (1 frame decoded). That is the
issue's freeze. On 1.6.3 it is absent:

| 1.6.3, Xvfb, two peer connections in one process | call 1 | call 2 | call 3 |
|---|---|---|---|
| set-up total (camera to both ICE connected) | 959 ms | 884 ms | 849 ms |
| of which `getUserMedia` (UI blocked all of it) | 775 | 752 | 746 |
| `A.setLocalDescription` / `B.setLocalDescription` | 13 / 61 | 9 / 13 | 9 / 9 |
| hang-up total | 305 ms | 285 ms | 260 ms |
| of which `track.stop(video)` (UI blocked all of it) | 260 | 260 | 240 |
| `A.close` / `B.close` | 4 / 13 | 3 / 2 | 4 / 3 |
| worst UI timer gap outside those two calls | under 70 ms | | |

Four runs, 3 to 6 calls each, agree: `getUserMedia` blocks for 0.63 to
1.15 s and `track.stop(video)` for 0.20 to 0.28 s on every call, the first
included. Every other call stays under 180 ms (`setLocalDescription` 9 to
160 ms, `close` 2 to 90 ms). Neither grows from one call to the next.
One more thing was seen in every call, the first too: one frame gap of
0.63 to 1.2 s about half a second after ICE connects, with no timer gap.
The UI isolate kept running, so something else held up the raster side.
It is probably Xvfb's software GL taking the encoder's start-up load, but
check on a real display.

**Provisional verdict on 2070, Linux: not reproduced with flutter_webrtc
1.6.3.** The same harness shows the 10 s freeze on 1.4.1, so its absence
on 1.6.3 is a real result and not a blind spot. The issue is still open
upstream, and no changelog entry claims a fix; it most likely went with
the move to libwebrtc m150 in 1.6.1. The pin must therefore be 1.6.3 or
later, and a pin bump needs this check run again. What is left is about
1 s of UI stall when the camera opens and about 0.25 s when it closes.
The call screen should be up before `getUserMedia` is called, so the
stall does not land on a tap.

**2225, confirmed in its long-lived form.** libwebrtc's capture fd has no
close-on-exec: `/proc/self/fdinfo` gives `flags: 0104002` for
`/dev/video0`. Two cases were run, each with the camera open in a call:

- `tailscale status` through `Process.run`, which exits in 24 to 129 ms.
  The call ended and the next call's camera opened and sent video (65
  frames decoded in 4 s). **A child that has exited does no harm.**
- `sleep 20` through `Process.start`, still running. `/proc/<child>/fd/26`
  pointed at `/dev/video0`. The call ended, and the next call opened the
  camera *with no error*: `getUserMedia` returned in 630 ms with a video
  track. But no video frame was ever sent (no inbound video at either
  peer), and `track.stop(video)` took 1 ms instead of about 250. **The
  failure is silent.** Once the child was killed, the following call had
  video again.

So the spec's rule stands as written: on Linux, no child process may be
running at the moment the camera is opened, or every child has to be
started with the camera's descriptor closed. The camera-open path should
also check that frames arrive and report "camera busy" if none come
within about 2 s, since `getUserMedia` will not.

**Candidates.** With no ICE servers, only host candidates are gathered (no
srflx, no relay), udp and tcp, on every interface: docker bridges, the
LAN, 10.11.12.28, two global IPv6 addresses, and the two tailnet ones.
The filter passed on only the tailnet pair (in one run, 24 kept against
84 dropped over 6 calls). Two findings for ticket 03/04:

- **Filtering what is signalled is not enough.** libwebrtc still binds
  its other interfaces locally and sends checks from them to the remote
  tailnet candidates. In the first run (default bundle policy), the
  nominated IPv4 pair was B's *peer-reflexive* 192.168.2.131 to A's host
  100.91.111.54. On one machine that is loopback, so it does not tell us
  whether such a packet would cross the tunnel between two machines. With
  `bundlePolicy: max-bundle` and `rtcpMuxPolicy: require`, every selected
  pair afterwards (21 calls) had tailnet addresses on both sides, mostly
  over IPv6. The real implementation should use max-bundle and read the
  selected pair from `getStats` (the `transport` report's
  `selectedCandidatePairId`). If either end is not a tailnet address, it
  should end the call as failed.
- **TCP.** One of three two-process calls settled on a TCP pair. Calls
  should use `tcpCandidatePolicy: disabled`, so media never sits behind
  TCP's head-of-line blocking inside WireGuard.

**VP8.** `getRtpSenderCapabilities('video')`, VP8 first and H.264 removed,
then `setCodecPreferences` on each transceiver. The offer's `m=video`
lists payload 96 (VP8) first, and every call decoded `video/VP8` at
640x480; audio was `audio/opus`.

**Two processes.** Answer and offer as two app instances on this machine,
signalling over TCP to 100.91.111.54:47000, 3 calls (the answer side
without a camera, since one webcam cannot be opened twice). Each call
connected in 0.9 to 1.0 s and decoded about 133 VP8 frames in 8 s, with
Opus both ways. This is the mode the phone run uses.

**Sizes** (the shipping app copied, with `flutter_webrtc: 1.6.3` added
and nothing else changed; release builds, bytes):

| | before | after | growth |
|---|---|---|---|
| Linux bundle (`du -sb bundle`) | 28 317 071 | 54 856 387 | +26.5 MB (libwebrtc.so alone is 24.2 MB) |
| APK arm64-v8a | 21 470 156 | 33 988 310 | +12.5 MB |
| APK armeabi-v7a | 18 899 050 | 25 944 946 | +7.0 MB |
| APK x86_64 | 23 019 797 | 39 420 956 | +16.4 MB |

These match ADR-0016's estimates (about 25 MB and 12 MB).

**Not tested here:** a call with Android in either direction, a call
between two machines over WireGuard (here both ends were the same host),
DERP, and the frame gap on a real display. The procedure for the user,
with a phone on the tailnet and the laptop at a real display:

1. Phone: in the Tailscale app, make sure the spike is not excluded by
   split tunnelling. Note the phone's tailnet IPv4 address,
   `tailscale status`.
2. Phone answers, Linux calls. From `companion/spike_webrtc`, with the
   phone on USB: `flutter run --release --dart-define=MODE=answer
   --dart-define=SIGNAL=0.0.0.0:47000 --dart-define=ROUNDS=3`. Allow camera
   and microphone when asked. Then on the laptop: `flutter run -d linux
   --release --dart-define=MODE=offer
   --dart-define=SIGNAL=<phone-ip>:47000 --dart-define=ROUNDS=3`.
3. Linux answers, phone calls. The laptop runs `MODE=answer
   SIGNAL=0.0.0.0:47000`, and the phone runs `MODE=offer
   SIGNAL=100.91.111.54:47000`.
4. In each `[spike]` log, read the `candidate ... KEEP/drop` lines, the
   `set-up total` lines, and the `stats:` lines. Those show the codec,
   frames decoded, audio packets, and the selected pair, whose two ends
   must both be tailnet addresses. Check that audio is heard both ways.
5. DERP: repeat steps 2 and 3 with the phone on mobile data and WiFi off.
   During the call, run `tailscale ping <phone-ip>` on the laptop: it
   should say `via DERP(...)`. If it goes direct anyway, restart
   tailscaled with `TS_DEBUG_ALWAYS_USE_DERP=true` (`sudo systemctl edit
   tailscaled`) and remove the setting afterwards.
6. On the laptop, also run the default loop mode (`flutter run -d
   linux --release` with no defines) at the real display. Check whether
   the 0.6 to 1.2 s frame gap after connect appears there.

The go/stop box is the user's to tick once the phone run is in. On the
Linux evidence there is no reason to stop: 2070 does not happen on 1.6.3,
and 2225 has a known rule and a check.

### The phone run (2026-11-04, Pixel 8 on Android 17, the laptop at its X11 display)

The spike as committed, with an outgoing-video line added to its stats
(packets, average size, frames encoded). Three calls each way, 4 s held,
the spike's own TCP signalling. Every call: VP8 and Opus, 640x480.

| Path (`tailscale ping`) | Laptop calls phone | Phone calls laptop |
|---|---|---|
| WiFi, direct on the LAN | 3/3, video both ways, 0 lost | 3/3, video both ways (phone awake) |
| Mobile data, via DERP ("direct connection not established") | 3/3, video both ways, 0 lost | |
| Mobile data, direct over the carrier's IPv6 | | 3/3, video both ways, 0 lost |

- **2070 does not show here either.** Set-up 1.0 to 2.3 s, the first
  call the slowest on every path; the phone's worst UI stall in a call
  127 ms, its `getUserMedia` 0.24 to 1.2 s.
- **Android gathers no candidate on Tailscale's interface.** The phone
  has `tun1` (100.92.9.101, fd7a:115c:a1e0::d801:966, MTU 1280), but
  libwebrtc offered only loopback, `wlan0` and the carrier's addresses,
  all dropped by the filter. Every call connected through a
  peer-reflexive candidate: the phone's checks reach the laptop's
  tailnet address through the VPN (which is not bypassable), and each
  side then sees the pair as tailnet at both ends (phone: local prflx
  fd7a:...d801:966 -> remote host fd7a:...3936:6f36). Ticket 05 must
  not need an Android tailnet candidate to connect, and its off-tailnet
  pair check must pass on a prflx pair (it does on these addresses);
  a phone calling a phone, where neither side has a tailnet candidate
  to signal, is untested and may not connect at all.
- **A locked or sleeping phone kills a foreground-less app's sockets.**
  Twice the phone's 30 s screen timeout ended a run: about 5 s after the
  lock, Android logged `Destroyed live tcp sockets for uids={10298}` and
  then BLOCKED for the spike, and its media died with the signalling.
  The companion app keeps its listening foreground service, which
  should keep its network on a locked phone; ticket 01's run on a
  locked phone is where that is checked. In the run where the phone
  slept mid-way it also received no video from the laptop; awake, on
  the same WiFi, it did on every call.
- The spike still gathers TCP candidates, and some calls settled on a
  tailnet TCP pair; the app admits UDP only (ticket 03).
- The laptop encodes about 17 fps from its webcam on every path,
  `qualityLimitationReason` none: the camera's rate, not the network.

**Verdict: go**, given by the user on 2026-11-04 after the phone run.
