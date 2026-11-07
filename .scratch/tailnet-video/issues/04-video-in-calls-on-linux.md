# 04: Video in calls on Linux

**What to build:** in a call between two Linux companion apps either side switches its camera on or off at any time, from the first ring. A conversation with a video-capable contact has a Video call button (absent for the T-Deck and older apps); a call offered with the camera on shows "Video call" on the ring screen and in its notification, with Answer with video, Answer (camera stays off) and Decline; a missed one is "Missed video call". The call screen shows the other side's video full size, one's own in a corner, the camera switch (only on a WebRTC call), Switch camera when there is more than one camera, and a tile saying Camera off, Camera paused or No camera. A machine with no camera still sees the other side. Video adapts to the path, capped at 1280x720, 30 fps; switching never renegotiates. See `../spec.md`, user stories 1 to 23.

**Blocked by:** 03

**Status:** in-review (the by-hand run between two machines is the user's)

- [x] The Video call button by capability; a call started from it rings with `video` and the caller's camera on
- [x] The ring screen's three buttons for a `video` offer, two otherwise; a plain Answer leaves the camera off; the camera never turns on without a tap
- [x] The camera switch replaces the track and sends `call_camera`; the other side's tile follows `on`, `off`, `paused` and `none`; Switch camera cycles cameras when there are two or more
- [x] No camera on one side: it sends `none` and still shows the other side's video
- [x] Capture and sending capped at 1280x720, 30 fps
- [x] Notifications say "Video call from" and "Missed video call" when the offer had `video`
- [x] While a camera is on, the app starts no child process, unless ticket 02 showed its short-lived ones do no harm
- [x] The app's call tests, with the fake media engine: the button present or absent by capability, the ring screen's buttons, the plain Answer, the switch, Switch camera with one and two cameras, the tile's states
- [ ] By hand, written here: a video call between two Linux machines, both ways, camera switched mid-call; one machine without a camera

## Comments

**2026-11-04, implementer.** Branch `tv-04-linux-video`, from
`tailnet-video` at f581077. Every box but the last is done and tested;
the last needs a second machine.

What was built:

- **Core**: `ChatPeer.videoCapable(key)`, true when the contact's last
  proven hello this run announced `call:webrtc` and ours does. Nothing is
  stored, so after a restart the Video call button shows once the app has
  been connected to that contact (it dials its contacts at start). The
  protocol is unchanged.
- **Engine** (`call_engine.dart`, `webrtc_engine.dart`): `cameras()`
  (flutter_webrtc's `videoinput` devices, in its order), `setCamera(id |
  null)` (replaces the video transceiver's track; the old camera is
  released first, as a phone's two cameras may not open together),
  `remoteVideo()` / `localVideo()` (RTCVideoView), and `videoStats()`
  (frames sent and received, sizes, fps, what limits the sending, from
  getStats). A camera is asked for 1280x720 at 30 fps at most
  (`deviceId`, and Linux's `optional: [{sourceId}]`), and the sender's
  encodings get `maxFramerate` 30. **2225's check**: a camera whose first
  frame does not reach our own renderer within 2.5 s throws
  `CameraUnavailable`; the app turns the camera off, sends `off`, and the
  call screen says "The camera is off: no picture came from it. Another
  program may be using it."
- **Talk**: `startCall(box, video:)`, `answer(id, video)`,
  `setCameraOn`, `switchCamera`, `cameras`, `cameraOn`, `cameraProblem`.
  At the media's start it reads the cameras and sends `none` without one,
  `on` (and opens it) if Video call or Answer with video asked for it, else
  `off`. A camera switched while the call is still ringing is applied at
  the answer. Notifications say "Video call from" and "Missed video call";
  Android's ringer is passed `video` too (its Kotlin side is ticket 05's).
  At the end the log has `call <id>: video sent N frames at WxH, ...`.
- **Child processes** (`child_processes.dart`): the app's two, `tailscale
  status` (the 30 s refresh, discovery, and a contact's `peers_ask`) and
  PipeWire's echo canceller (only for `ws-opus` calls, which have no
  camera), go through one gate: none starts while a camera is on (the
  status is not read again meanwhile; the last one stands), and a camera
  opens only once those running have exited, 2 s at most. I gated the
  short-lived `tailscale status` too, though ticket 02 found an exited
  child harmless: it is cheap, and a run that hung would hold the camera.
- **Screens**: the conversation's Video call button (`video-call`) for a
  video-capable contact; the ring screen's "Video call" with Answer with
  video, Answer and Decline; during a WebRTC call, the other side's video
  full size while its camera is `on`, a tile saying Camera off, Camera
  paused or No camera otherwise, ours in the top right corner, the camera
  switch (No camera, disabled, without one) and Switch camera with two or
  more cameras while ours is on.
- **A fix on the way**: `showCall` kept a flag that only the route's
  completion cleared, so a call screen torn down with its navigator left
  every later call without a screen. It now keeps the route and checks
  `isActive`. Tests had passed only by closing every call screen.

Verified here: core `dart test` (99), app `flutter test` (132),
`flutter analyze --fatal-infos`, `dart format`, the conformance suite
against the headless core (37 of 38, `call_ended_by_cellular` skipped as
before), `flutter build linux --release`, `flutter build apk --release`.

**The real engine, in one process** (`tool/webrtc_call_check.dart`, under
Xvfb, this laptop's Integrated_Webcam_HD, release build, PASS): after the
two voice calls of ticket 03, a third call: offered with `video` true, the
callee answered plainly and said `off`, the caller's camera opened at
"1280x720 at 30 fps" (its capture settings) and sent 63 frames in 4 s,
all received; 12 s in, 196 frames, sent at 480x270 and climbing from
320x180, 17 fps, "limited by bandwidth" (WebRTC's ramp from its starting
rate). Then the caller's camera off (`off` at the other side), the
callee's on: the same camera reopened in the same process at once and 63
frames went the other way. Media over `fd7a:115c:a1e0::3936:6f36` both
ends, only tailnet UDP candidates sent. Switch camera across two devices
could not be run: `/dev/video0` and `/dev/video1` are the one webcam's two
nodes, and flutter_webrtc lists one camera, so Switch camera is not shown;
it is tested only with the fake engine.

**Left for the hardware run (ticket 06), by hand:**

1. Two Linux machines on the tailnet, this branch's build on both: a
   Video call each way. Check the ring screen says Video call with three
   buttons, the notification "Video call from ..." with the app in the
   background, and "Missed video call" when let ring out. Answer plainly:
   the caller's picture shows, ours does not, the caller sees "Camera off".
   Switch the camera on and off mid-call from each side; each sees the
   other's picture follow. With `--verbose`, the log's `camera on: WxH at
   N fps` and the `video sent ... received ...` line at the end; on a
   direct WireGuard path the sending size should reach 1280x720, or say
   what limits it.
2. A machine with two cameras (a USB camera beside the built-in one):
   Switch camera shows and goes from one to the other and back.
3. A machine without a camera (or one with its camera unplugged): the
   switch says No camera, the other side sees "No camera", and it still
   sees the other side's picture.
4. A second video call in a row on each machine: the camera gives a
   picture again (2225, and the 2070 freeze), and the UI's stall when the
   camera opens (about 1 s on Xvfb) is acceptable on a real display.
5. A call to the T-Deck from each: no Video call button, `ws-opus` as
   before.
