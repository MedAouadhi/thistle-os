# Video in Tailnet chat calls

Status: ready-for-agent

`CONTEXT.md` defines **Call** and **Video-capable**, beside Tailnet chat's
**Chat peer**, **Contact** and **Identity key**. ADR-0015 (peer to peer,
the tailnet trusted once) bounds it; `.scratch/tailnet-chat/spec.md` is the
chat and voice this builds on, and protocol v1's section 5 reserved room for
it. ADR-0016 records WebRTC beside `ws-opus`, the prebuilt binary's
licences and codecs, and the tailnet-only candidates.

## Problem Statement

The companion app on the user's laptop and phone calls the T-Deck and other
companion apps, voice only. Between two companion apps, both on machines with
cameras, the user cannot see the person they are talking to, and has to
leave Tailnet chat for another messenger, one that goes through a company's
servers, to do so. The T-Deck has no camera and could not show video, so
whatever is added must leave its calls as they are.

A call to a locked Android phone does not ring like a call either: the
phone vibrates once, as for a message, and nothing comes on the screen.
The call notification asks to fill the screen, but the only window it can
open is the app's, which is kept off the lock screen so that the chats are
never shown there; the notification alerts once, and the app plays its own
two-tone ring. Every chat app the user has rings with the phone's ringtone
and a call screen over the lock screen.

## Solution

Calls between two companion apps on Linux and Android carry video. There is
still one kind of call: either side switches its camera on or off whenever it
likes, from the first ring on, and a call started with the camera on is
simply a call whose camera was on from the start. Whether a call can carry
video is decided by what both chat peers announce, never by the machine they
run on: the T-Deck, and companion apps from before this, are not
video-capable, and calls with them are the voice calls of today.

Between two video-capable chat peers a call runs on WebRTC, with or without a
camera on, over the tailnet's addresses only. Calls with the T-Deck stay on
`ws-opus`.

On Android, every call that comes in, from a companion app or the T-Deck,
voice or video, rings like a phone call: the screen turns on, a call screen
shows over the lock screen with nothing of the chats behind it, and the
phone's ringtone loops and vibrates as its ringer switch says, until the call
is answered, declined or given up.

## User Stories

### Starting and answering

1. As the user, I want a Video call button in a conversation with a
   video-capable contact, so that I can call with my camera on from the
   first ring.
2. As the user, I want the Call button to stay, so that I can call with my
   camera off and decide later.
3. As the user, I want no Video call button in a conversation with my
   T-Deck, or with a contact whose app cannot do video, so that I am not
   offered what cannot work.
4. As the user, I want an incoming call whose caller has the camera on to
   say Video call, on the ring screen and in its notification, so that I
   know I will be seen once I turn my camera on and see them now.
5. As the user, I want to answer a video call with my camera on or with it
   off, or decline it, so that I choose whether I am seen.
6. As the user, I want my camera never switched on without my tap, so that
   answering a call never shows me by surprise.
7. As the user, I want a call from a contact with the camera off to ring as
   it does today, with Answer and Decline, so that nothing changes when no
   one asked for video.
8. As the user, I want a missed call whose caller had the camera on to be
   notified as a missed video call, so that I know what I missed.
9. As the user, I want to be asked for camera permission the first time I
   turn the camera on, not at install, so that the request makes sense when
   it comes.
10. As the user, I want a call to carry on without my camera when I refuse
    camera permission, so that refusing does not cost me the call.

### During a call

11. As the user, I want to switch my camera on and off at any point in a
    call with a video-capable contact, so that I can show something and
    stop.
12. As the user, I want to see the other side's video whenever their camera
    is on, so that I see who I am talking to.
13. As the user, I want to see my own camera's picture small in a corner,
    so that I know what I am sending.
14. As the user, I want to see the other side's video even when I have no
    camera, so that a desktop without a webcam still has the picture.
15. As the user, I want the other side's tile to say whether their camera
    is off, paused or absent, so that a still picture is never mistaken for
    a frozen call.
16. As the user, I want a Switch camera button when my phone or laptop has
    more than one camera, so that I can show what is behind the phone or
    use my USB camera.
17. As the user, I want my phone to start on its front camera, and my
    laptop on the first camera it finds, so that the usual camera is the
    first one.
18. As the user, I want mute, volume and hang up to work as they do today,
    camera or not, so that the call screen stays familiar.
19. As the user, I want the video to adapt to the connection, up to 720p,
    so that a call over a relay or mobile data keeps going rather than
    stalling.
20. As the user, I want a call through Tailscale's relay to work, worse but
    working, so that I can call when no direct path exists.
21. As the user, I want a call that loses its connection for 10 seconds to
    end and say so, so that I am not left looking at a frozen picture.
22. As the user, I want a call that cannot connect its media to end with an
    error rather than ring on silently, so that I know to try again.
23. As the user, I want calls between my companion apps to cancel the echo
    of my speaker, so that the other side does not hear themselves.

### On Android

24. As the user, I want leaving the app during a call to pause my camera
    and keep the voice going, so that I can check something else without
    hanging up.
25. As the user, I want my camera to come back when I return to the call,
    so that I need not switch it on again.
26. As the user, I want the other side told my camera is paused while I am
    away from the app, so that they know why my picture stopped.
27. As the user, I want a call with video on screen to play on the
    loudspeaker, with the screen kept on, so that I can hold the phone in
    front of me.
28. As the user, I want a call with both cameras off to go back to the
    earpiece, with the screen darkened at my ear, so that a voice call is
    still a phone call.

### Calls with the T-Deck and older apps

29. As the user, I want calls between my companion app and my T-Deck to be
    exactly as they are today, so that the device is unaffected.
30. As the user, I want a call with a contact whose companion app predates
    video to work as a voice call, so that updating one app does not break
    calls with the other.
31. As the user, I want a second call to get busy while I am in a call with
    video, as with any call, so that an ongoing call is not interrupted.

### Privacy and trust

32. As the user, I want a call's media to go only over my tailnet, never
    over my LAN or the internet directly, so that it is admitted and
    encrypted like every other chat frame.
33. As the user, I want no STUN or TURN server contacted, so that no third
    party learns of my calls.
34. As the user, I want video offered only to contacts, as calls are, so
    that a stranger cannot call me with a camera.

### A call to a locked phone (Android)

42. As the user, I want a call to my locked phone to turn the screen on and
    show a call screen over the lock screen, so that I see who is calling
    without unlocking.
43. As the user, I want to answer or decline from that screen without
    unlocking, and Answer with video when the caller's camera is on, so that
    it works like a phone call.
44. As the user, I want nothing but the call on that screen, and to be back
    at the lock screen when the call ends, so that someone holding my locked
    phone cannot read my chats.
45. As the user, I want the phone's own ringtone to ring, again and again,
    until I answer, decline or the caller gives up, so that a call sounds
    like a call and not a message.
46. As the user, I want the ring to follow my ringer switch and Do Not
    Disturb's rules for calls, so that a silenced phone stays silent.
47. As the user, I want the phone to vibrate while it rings when it is on
    vibrate or set to ring and vibrate, so that I feel a call in my pocket.
48. As the user, I want a call to ring this way whether it comes from my
    laptop, another companion app or my T-Deck, and with or without video,
    so that every call is the same.
49. As the user, I want a call that comes while the phone is unlocked but
    the app is in the background to show as a call notification with Answer
    and Decline and ring the same way, so that I can take it from anywhere.
50. As the user, I want Settings to tell me when Android stops calls from
    filling the screen, with a button to the page that allows it, so that I
    can fix it once.
51. As the user, I want a call to still ring as a call notification when
    that permission is missing, so that I never miss one for it.

### Development

35. As a developer, I want the protocol document to describe `call:webrtc`,
    `call_rtc` and `call_camera` with test vectors, so that a third
    implementation can follow it.
36. As a developer, I want the reference chat peer's conformance suite to
    cover the WebRTC call's signalling against the companion's Dart core,
    so that the protocol is checked without cameras.
37. As a developer, I want the same suite run against the simulator to show
    the T-Deck still answers `ws-opus`, so that the device is known
    unaffected.
38. As a developer, I want the app's call screen tested against a fake call
    media engine, so that the camera switch, the tiles and the audio route
    are tested without WebRTC.
39. As a developer, I want the libwebrtc binary and the Android artifacts
    pinned by hash, so that a changed download fails the build instead of
    shipping.
40. As a developer, I want a spike to establish, before the rest is built,
    whether flutter_webrtc's known Linux bugs bite this app, so that the
    plan is changed early if they do.
41. As a developer, I want the log to say which transport a call took, its
    candidates' kinds, camera changes and why a call ended, so that a bad
    call can be read from the log.

## Implementation Decisions

### The protocol (still version 1)

- **A capability, `call:webrtc`.** A chat peer announces it when it can
  take part in a WebRTC call, camera or not. Companion apps on Linux and
  Android announce it; the T-Deck does not, and needs no change.
- **The offer.** `call_offer` lists `webrtc` before `ws-opus` when both
  hellos announce `call:webrtc`, and gains `video`, true when the caller's
  camera is on as it rings. A peer without `call:webrtc` is offered
  `ws-opus` alone, as today. `call_answer` picks `webrtc` whenever it is
  offered and the callee announced it.
- **Signalling, once.** After `call_answer` picks `webrtc`, the caller sends
  its SDP offer and the callee its SDP answer, each as a `call_rtc` frame on
  the chat connection, and both trickle ICE candidates the same way:

  ```json
  {"type":"call_rtc","call":"<32 hex>","sdp":{"type":"offer","sdp":"v=0..."}}
  {"type":"call_rtc","call":"<32 hex>","candidate":{"candidate":"candidate:...","sdpMid":"0","sdpMLineIndex":0}}
  ```

  The SDP always negotiates one audio and one video track, both ways, so
  switching a camera on, off or to another camera replaces a track and
  never renegotiates: there is no glare to resolve.
- **Camera state.** `call_camera` says what one side's camera is doing:
  `on`, `off`, `paused` (its app left the foreground) or `none` (no camera,
  or no permission). It is sent at the answer and at every change, and
  drives the other side's tile.

  ```json
  {"type":"call_camera","call":"<32 hex>","camera":"paused"}
  ```

- **Tailnet only.** No ICE servers: no STUN, no TURN. A side sends only its
  candidates whose address is in 100.64.0.0/10 or fd7a:115c:a1e0::/48, and
  ignores any other it receives, in `call_rtc` or inside an SDP. Media goes
  over WireGuard like every other frame; WebRTC's own DTLS-SRTP inside it
  cannot be turned off and is not relied on. libwebrtc binds every
  interface and may still select a pair from an address it never sent
  (tickets 02 and 03), so while the media is connected the app reads the
  selected pair from WebRTC's statistics, 2 s after it first connects (on
  one machine the first pair is a peer-reflexive LAN address, gone within
  half a second) and every 5 s after, and ends the call with `call_end`
  `error` if either end is outside those ranges.
- **Ends.** The 45 s ring and `busy` rules are unchanged. A WebRTC call
  that is not connected 15 s after the answer, or that stays disconnected
  for 10 s, is ended by either side with `call_end` `error` (connected) or
  `timeout` (never connected), and says so. It never falls back to
  `ws-opus` mid-call. `call_rtc` and `call_camera` for an unknown call id
  are ignored. Neither is queued: calls are never in the outbox.
- **Codecs.** VP8 is put first in the codec preferences for video, Opus for
  audio; H.264 is never preferred. The frames are WebRTC's, not protocol
  v1's binary frames; kind `02` stays `ws-opus`'s.

### The companion core (pure Dart)

- **Policy** decides the transport list for an offer and the pick for an
  answer from both hellos, whether a candidate is admitted (the tailnet's
  ranges), and when a WebRTC call has failed to connect or been
  disconnected too long. Host-tested like today's call rules.
- **The peer** carries `call_rtc` and `call_camera` between the chat
  connection and its user, the app, as opaque SDP and candidate values: the
  core does no WebRTC itself and keeps no Flutter dependency. It reports a
  call's transport, the other side's camera state and the offer's `video`
  in its call events.

### The companion app

- **One media dependency, flutter_webrtc**, with its prebuilt libwebrtc,
  on Linux and Android. The Linux binary is fetched at a pinned release and
  checked against a SHA-256 committed in the repository before the Flutter
  build configures, locally and in CI; the Android artifacts are pinned by
  Gradle dependency verification. Its licences (MIT wrapper, BSD
  libwebrtc, OpenH264 BSD-2 and Chromium's FFmpeg LGPL-2.1 inside the
  Linux binary; no GPL) go into the third-party licences and ADR-0016.
- **A call media engine**, the app's one new seam: an interface beside the
  audio devices, which a call's controller drives (start with or without
  the camera, set the remote description, add a candidate, switch the
  camera on, off or to the next one, mute, volume, end) and which reports
  local candidates, connection state, and the local and remote video to
  render. Its implementation is over flutter_webrtc; a fake stands in for
  tests. A `ws-opus` call keeps today's audio devices and media path.
- **The call screen** gains the other side's video full size, one's own in
  a corner, the camera switch and Switch camera (shown with more than one
  camera), and a tile that says Camera off, Camera paused or No camera. The
  camera switch is shown only on a WebRTC call. The ring screen offers
  Answer with video, Answer and Decline when the offer has `video`, else
  Answer and Decline; a plain Answer leaves the camera off. Its echo line
  names WebRTC's canceller on a WebRTC call. The conversation gets a Video
  call button for video-capable contacts.
- **Notifications** say Video call from, and Missed video call, when the
  offer had `video`.
- **Permissions.** Android asks for the camera the first time it is
  switched on, as the microphone is asked for today; a refusal sends
  `call_camera` `none` and the call goes on.
- **Echo cancellation.** A WebRTC call relies on libwebrtc's own
  processing (in-process on Linux, the platform's on Android); the Linux
  app starts no PipeWire echo canceller for it. A `ws-opus` call keeps
  today's.
- **Android.** Leaving the activity pauses the camera (`call_camera`
  `paused`) and keeps audio in the listening service, as a voice call does;
  returning resumes it. There is no picture-in-picture and no camera
  foreground-service type. While either side's video is on screen, audio
  plays on the loudspeaker, the proximity screen-off is released and the
  screen is kept on; with both cameras off, the earpiece and the proximity
  lock come back.
- **Quality.** Capture and sending are capped at 1280x720, 30 fps; WebRTC's
  congestion control does the rest. No rule for mobile data.
- **Child processes on Linux.** The camera's device stays open in any child
  process started while it is open (flutter_webrtc issue 2225), so the
  app starts none while a camera is on, or the spike shows that its
  short-lived ones do no harm.

### A call to a locked phone (Android)

- **For every incoming call**, `ws-opus` or WebRTC, voice or video. Nothing
  changes on Linux, which keeps its notification and two-tone ring.
- **A call window of its own**: an activity apart from the main one, shown
  over the lock screen and turning the screen on, on the one Flutter
  engine, that shows the call screen and nothing else. The full-screen
  intent of the call notification opens it; answering from it needs no
  unlock. When the call ends it finishes, so a locked phone is back at the
  lock screen; only unlocking reaches the chats. The main window stays off
  the lock screen, as before.
- **A call-style notification** (Android's own, with the caller, Answer and
  Decline), on the `calls` channel, while a call rings: on the lock screen
  it is what opens the call window, and with the phone unlocked and the app
  in the background it is a heads-up notification. Its Answer and Decline
  reach the engine whose chat peer has the call.
- **The ring is the phone's**: its default ringtone, looping, on the
  ringtone stream, with a repeating vibration, both by the phone's ringer
  mode and Do Not Disturb's call rules, from the offer until the answer, the
  decline, the caller's `call_end` or the 45 s timeout. The app's
  synthesised ring is no longer played on Android. The call notification is
  silent itself, so nothing rings twice.
- **The full-screen permission.** Where Android withholds it (Android 14
  and later, for apps that are not calling apps), the app checks at start
  and when a call comes; lacking it, Settings shows "Calls can't fill the
  screen" with a button to Android's page for it, and calls ring as the
  call-style notification alone.
- **Not Telecom.** Calls are not registered with Android's telephony
  framework (a self-managed ConnectionService): no answering from a
  Bluetooth headset or a car, and the system does not arbitrate between a
  tailnet call and a cellular one.
- **The seam** is the notifier the call controller already drives (ringing
  and stop ringing): its Android implementation gains the call window, the
  call-style notification and the ringtone; the call tests drive a fake of
  it, as they do now.

### Order of work

1. **A spike, blocking the rest**: a tailnet-only VP8 call between the
   Linux app and the Android app, with candidates filtered; whether the
   UI freezes for about 10 s on the second call (flutter_webrtc issue
   2070) and whether the camera stays busy (2225); the APK's growth. If
   the freeze is confirmed, the plan stops for a decision.
2. The protocol document, its vectors, the reference chat peer and the
   core.
3. The Linux app: the media engine, the call screen, the build's pinning.
4. The Android app: the camera's lifecycle, the audio route, Gradle
   verification.
5. Android's incoming call: the call window over the lock screen, the
   call-style notification, the phone's ringtone, the full-screen
   permission. It does not need video and does not wait for the spike.
6. The manual hardware run.

## Testing Decisions

- **A good test drives a chat peer through its outside**: frames on the
  wire, the call screen's widgets and what they show, the events a call
  reports. It does not reach into a peer connection or the core's state.
- **The wire.** The reference chat peer (stdlib Python) learns
  `call:webrtc`, `call_rtc` and `call_camera`, with SDP and candidates as
  opaque strings. The conformance suite run against the companion's Dart
  core, headless, covers: the offer's transports with and without the
  capability on either side; the answer's pick; the SDP and candidates
  relayed; a candidate outside the tailnet's ranges not sent and one
  received ignored; `call_camera` states; `video` on the offer; a call that
  never connects and one disconnected for 10 s ended; `busy` during a
  WebRTC call. Run against the simulator, it checks the T-Deck announces no
  `call:webrtc` and answers an offer of both with `ws-opus`. Prior art: the
  existing conformance suite and its simulator and headless targets.
- **The core's policy** is host-tested like today's call rules: the
  transport list and pick, candidate admission, the connect and disconnect
  timeouts.
- **The app.** The call tests drive two in-process chat peers, as the voice
  call tests do, with the fake call media engine: the Video call button
  present or absent by capability; the ring screen's three buttons for a
  `video` offer; a plain Answer leaving the camera off; the camera switch,
  Switch camera with one and two cameras, and the tile's states; a refused
  permission sending `none`; on Android, leaving the app sending `paused`
  and the audio route following the video. Prior art: the app's voice
  call and UI tests, and its fake audio devices.
- **Ringing on Android.** The call tests check, through the fake notifier,
  that a call coming in with the app in the background asks to ring with the
  caller and whether the camera is on, and that the answer, the decline, the
  caller's end and the timeout each stop it; the settings test, that the
  banner shows when the full-screen permission is missing. The call window,
  the ringtone and the lock screen are Android's, checked on the phone.
- **Test vectors** beside the protocol document get one of each new frame.
- **Hardware**, a written manual run in the tickets: Linux to Android and
  back, Linux to Linux, over WiFi directly and through DERP, with mobile
  data on the phone; a laptop without a camera; camera permission refused;
  leaving the app on Android; the speaker and the earpiece; echo with the
  laptop's own speaker; a call to the T-Deck from each, unchanged; a second
  call in a row on Linux, for the freeze. On a locked phone: a call from the
  Linux app and one from the T-Deck turn the screen on and ring with the
  phone's ringtone; answered without unlocking, the chats are not reachable
  and the lock screen is back after the hang-up; the ringer on vibrate and
  on silent, and Do Not Disturb; the full-screen permission revoked in
  Android's settings, and the banner.

## Out of Scope

- Video on the T-Deck, in either direction; any firmware change.
- Screen sharing, group calls, recording, effects and backgrounds.
- Windows, macOS and iOS builds of the companion app.
- Picture-in-picture on Android, and the camera running in the background.
- LAN or internet paths for media, STUN and TURN.
- A Settings choice of camera, microphone or speaker.
- A rule or setting for video over mobile data.
- Switching a call between `ws-opus` and WebRTC once it has begun.
- A call log in the companion app; missed calls stay notifications.
- Registering calls with Android's telephony framework (a self-managed
  ConnectionService): Bluetooth headset and car answering, and the system
  arbitrating between a tailnet call and a cellular one.
- Changes to how a call rings on Linux.

## Further Notes

- Every call between two companion apps moves from `ws-opus` to WebRTC,
  camera or not. That is the cost of a camera switch in every call, and
  the reason the spike comes first: flutter_webrtc's Linux freeze would hit
  voice calls too. Because the offer lists both transports, a build can
  stop offering `webrtc` and fall back to today's voice calls without a
  protocol change.
- The Linux libwebrtc binary is about 25 MB, and Android's adds about
  12 MB to each split APK.
- On Android, an app excluded from the Tailscale app's split tunnel gathers
  no tailnet candidate and cannot connect a WebRTC call; it cannot chat
  either.
- A protocol v1 peer that ignores `call:webrtc` never sees `call_rtc` or
  `call_camera`, since neither is sent without a WebRTC call; one that
  receives either anyway answers `error unsupported`, as for any unknown
  type.
