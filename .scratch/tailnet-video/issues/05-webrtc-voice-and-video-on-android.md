# 05: WebRTC voice and video on Android

**What to build:** the Android companion app announces `call:webrtc` and calls the Linux app and other Android apps on WebRTC, voice and video, as ticket 04 does on Linux; calls with the T-Deck stay `ws-opus`. The camera is asked for the first time it is switched on, and a refusal sends No camera while the call goes on. Leaving the app pauses the camera (the other side sees Camera paused) and keeps the voice in the listening service; returning resumes it. While either side's video is on screen, audio plays on the loudspeaker with the screen kept on and no proximity screen-off; with both cameras off, the earpiece and the proximity lock come back. Android starts on the front camera, with Switch camera. The lock-screen call window from ticket 01 offers Answer with video when the offer has `video`. The Android artifacts are pinned by Gradle's dependency verification. See `../spec.md`, user stories 24 to 28 and 43.

**Blocked by:** 01, 04

**Status:** in-review (the on-phone run is the user's)

- [ ] The call media engine runs on Android; a call between the Android app and the Linux app is WebRTC, voice and video both ways
- [x] Camera permission asked on first use; refused, `call_camera` `none` and the call continues
- [x] Leaving the activity sends `paused` and keeps audio; returning resumes the camera and sends `on`; no picture-in-picture, no camera foreground-service type
- [x] The audio route follows the video: loudspeaker and screen kept on while any video shows, earpiece and proximity lock otherwise
- [x] Front camera first; Switch camera to the back one
- [x] The lock-screen call window shows Answer with video for a `video` offer
- [x] Gradle dependency verification covers flutter_webrtc's Android artifacts; CI's Android build passes with it
- [x] The app's call tests, with the fake media engine: a refused permission sends `none`, leaving the app sends `paused`, the audio route follows the video

## Comments

**2026-11-04, implementer.** On `tailnet-video` (03d5429), from
51a0c5c. Every box but the first is done and tested
here; the first is a call on the phone, which could not be placed (below).

What was built:

- **Android announces `call:webrtc`** (`main.dart`): the app has the
  flutter_webrtc engine on Android as on Linux, so a call with another
  companion app is WebRTC and a call with the T-Deck stays `ws-opus`.
- **The camera permission** (`AppPermission.camera`, `Platform.kt`,
  `CAMERA` in the manifest, `android.hardware.camera` not required): asked
  the first time the camera is switched on in a call, Video call, Answer
  with video or the switch, never at the ring. Refused, the other side is
  told `none`, the call screen says "The camera is not allowed, so the
  other side sees No camera.", the banner says what it stops, and the call
  goes on; the switch asks again.
- **Away** (`ChatModel.away`, set in `app.dart` from the lifecycle):
  hidden or paused is away, resumed is back; inactive (a permission prompt,
  the notification shade, the way back) changes nothing, or the camera
  prompt would pause the camera it asks for. On Android
  (`cameraPausesAway`) a camera that is on is let go and told `paused`, the
  session and its voice going on, and comes back `on` with the app; a
  caller's camera open while it rings is let go and opened again the same
  way, and a call answered meanwhile starts `paused`. Linux is unchanged.
  The call keeps running where the ws-opus call does: the Flutter engine
  belongs to `Engines`, not the activity, and the listening service is in
  the foreground as using the microphone for the call (`rtc.begin`), with
  no camera type and no picture-in-picture.
- **The call route** (`CallRoute` in `call_engine.dart`, Android's
  `AndroidCallRoute` in `android_audio.dart`, `rtc.*` in `Audio.kt`):
  `begin` at the media's start (the listening service as using the
  microphone, the proximity lock, the earpiece), `video(shown)` when
  "either camera is on" changes, `end` at the media's end. With video,
  flutter_webrtc's audio switch to the loudspeaker (a Bluetooth or wired
  headset still first), the proximity lock released and
  `FLAG_KEEP_SCREEN_ON` on whichever window is attached (the main one or
  the call window, moved as they change). Without, the earpiece (headsets
  first) and the proximity lock. Audio.kt sets no audio mode for a WebRTC
  call: flutter_webrtc's audio switch does, and two savers of the mode
  would restore each other's.
- **Front camera first** (`frontFirst`, from flutter_webrtc's Android
  labels `Camera 1, Facing front, ...`; a Pixel lists the back one
  first), Switch camera to the back one; our picture mirrored only for a
  camera that does not face away (`mirrored`).
- **The call window over the lock screen** already drew ticket 04's ring
  screen; tested now with a `video` offer: Answer with video, Answer,
  Decline, the call alone. Its call-style notification says "Tailnet
  video call" and is marked as video (`CallStyle.setIsVideo`).
- **A fix on the way**: the ring screen's three buttons overflowed a
  phone's width by 19 px ("Answer with video" in one line); the round
  buttons are now 80 px wide with labels on up to two lines, aligned at
  the top.
- **Gradle dependency verification**:
  `android/gradle/verification-metadata.xml`, SHA-256 of every artifact
  the Android build fetches (745 components): `io.github.webrtc-sdk:android`
  150.7871.04, `com.github.davidliu:audioswitch` (JitPack), AGP, Kotlin,
  AndroidX, the Flutter engine's. A changed hash fails the build (tried:
  `Dependency verification failed ... android-150.7871.04.aar`). Written
  through `flutter build apk` itself (release and debug), with an empty
  Gradle cache and a copy of the Flutter SDK whose own Gradle plugin had
  not been built, as on CI: written by `./gradlew` directly, the file
  lacked the Kotlin BOM 2.2.20 that Flutter's Gradle plugin fetches when
  it is built, and a clean build failed on it. The README has the
  procedure. The CI job needed no change beyond a comment; Gradle reads
  the file itself.
- **Docs**: README (WebRTC, Video, Android, Tests), protocol 4.11 (a
  peer-reflexive tailnet end passes; Android signals none; two phones
  call on `ws-opus`, after the review), ADR-0016 (the Android pin, the tun1 finding, the camera
  pausing), THIRD_PARTY_LICENSES (the Android libwebrtc and audioswitch,
  Apache-2.0).

**The tailnet interface (ticket 02's finding 1).** Not fixable cheaply.
flutter_webrtc 1.6.3's only network option is `networkIgnoreMask`, whose
default ignores nothing (VPN included); it sets field trials only for
its own two features and calls `PeerConnectionFactory.initialize` itself,
which replaces any field trials set before it. libwebrtc's Android
network monitor (read from the 150.7871.04 AAR) already asks for VPN
networks by default (`requestVPN` true; `includeOtherUidNetworks` false
only matters for other users' networks), so no reachable switch is left
to try. So the phone signals no candidate of its own (every one it
gathers is outside the tailnet, and the core drops it), and the call
connects from the other side's: Android's checks reach the laptop's
tailnet address through the VPN, both sides see a peer-reflexive pair
with tailnet addresses at both ends, and the off-tailnet check passes on
it (ticket 02's phone run: local prflx fd7a:...d801:966). Two phones
have no candidate between them; after the review, the app on Android
announces `call:webrtc-prflx`, and a call between two phones that both
announce it is `ws-opus`, voice only (below). Written into the README,
protocol 4.11 and ADR-0016.

**The phone.** The Pixel 8 was attached over wireless adb but locked with
its PIN (`deviceLocked=1`), and the laptop's `default` profile, the
phone's contact, was held by the user's own running app (pid 2570029 on
`pts/5`, built from the main checkout's `tailnet-video` at 21:06), which
I did not stop. With no way to open the phone's chats and no free
identity the phone knows, no call was placed. Done on the phone: the
release APK (split, arm64, version code 2001 as installed) installed with
`adb install -r`, data kept; the app started (`am start`), its engine
up and `ListenService` back in the foreground (`types=0x200`, remote
messaging). Nothing else on the phone was changed. The camera permission
was not granted.

Verified here: core `dart test` (102), app `flutter test` (146, all; later
runs 145, the Secret Service test failing on "the keyring prompt was
dismissed" once the desktop's keyring had locked, which this branch does
not touch), `flutter analyze --fatal-infos`, `dart format`,
the conformance suite against the headless core (37 of 38,
`call_ended_by_cellular` skipped as before), `flutter build apk
--release` (97.4 MB; split arm64 34.3 MB), with verification, from an
empty Gradle cache and an unbuilt copy of the Flutter SDK (as CI), `flutter build linux --release`,
`tools/test_ci_workflows.py`.

**Left for the hardware run (ticket 06), on an unlocked phone:**

1. The phone calls the Linux app and the Linux app calls the phone,
   voice then video, over WiFi and over mobile data: the call connects
   (`media over prflx/udp fd7a:... to host/udp fd7a:...` on the Linux
   side's `--verbose` log), video both ways, and no call ends as "its
   media left the tailnet".
2. Video call from the phone: the camera prompt shows at the first
   switch-on; Allow, the front camera's picture in the corner, the
   laptop sees it; Switch camera to the back one (not mirrored) and
   back. Revoke the permission afterwards (`adb shell pm revoke
   org.thistleos.thistle_chat android.permission.CAMERA`), call again,
   Don't allow: the laptop's tile says No camera, the call goes on.
3. During a video call, Home on the phone: the laptop's tile says Camera
   paused, the voice goes on both ways; lock the phone for a minute
   (ticket 02's dead sockets), then come back: the camera comes back and
   the laptop sees it. The listening notification stays meanwhile.
4. The route: a voice call on the earpiece with the screen dark at the
   ear; the laptop's camera on moves the sound to the loudspeaker and the
   screen stays on with the phone held away; both cameras off brings back
   the earpiece and the proximity screen-off. With a headset: the sound
   stays in it either way.
5. On the locked phone: a Video call from the laptop rings with the
   call window offering Answer with video; answered so, without
   unlocking, the camera prompt (first time) shows over the lock screen
   and the call has video; the lock screen is back after the hang-up.
   The notification on the unlocked phone says "Tailnet video call".
6. A call from the phone to the T-Deck: no Video call button, `ws-opus`
   as before (earpiece, AcousticEchoCanceler).
7. Two phones, if a second is to hand: the call is `ws-opus`, voice
   only, with no Video call button on either (`call:webrtc-prflx` on
   both hellos), earpiece and AcousticEchoCanceler as with the T-Deck.

**2026-11-05, implementer: the review's fixes.** Branch
`tv-05-review-fixes`, from `tailnet-video` at 03d5429. Two phones call on
`ws-opus`: the app on Android announces `call:webrtc-prflx` beside
`call:webrtc` (its WebRTC gathers no tailnet candidate), and an offer
lists `webrtc` only when both hellos announce `call:webrtc` and not both
`call:webrtc-prflx` (protocol 4.8 and 4.11; `offers` in
`vectors/webrtc.json`; the reference peer, the core's `webrtcBetween`, a
contact's stored hello with it, schema 3; conformance
`webrtc_not_offered_between_two_phones`). An older app, with
`call:webrtc` alone, behaves as before; Linux and the T-Deck are
unchanged. The camera switched on while the app is away keeps only the
wish, told as `paused`, and opens on the return. The call window tells
Dart when it is resumed, and the app is not away while it is in front,
so Answer with video over the lock screen opens the camera at once.
`MainActivity` registers itself again on resume, so a video call keeps
the screen on in it after the call window closes. The call route chooses
no output at `begin`; the first `video(false)` after the media starts
picks the earpiece. Cleanups: one owner of away (`ChatModel.hidden` and
`away`, which Talk and WebrtcCall read), `cameraPausesAway` a property
of `CallRoute`, `Audio.kt`'s calls sharing `callStarted`/`callEnded`, one
permission helper in Talk, a refused or pictureless camera handled in one
place (`_noCamera`), the camera paused through `_applyCamera`, a
`Camera(id, facing)` value from `CallEngine.cameras`, `FakeCallRoute.now`
null before any output. Verified: core `dart test` (105), app `flutter
test` (152, the Secret Service one included), `flutter analyze
--fatal-infos`, `dart format`, `tools/test_tailnet_chat.py` (59), the
conformance suite against the headless core (38 of 39, one skipped),
`flutter build apk --release`, `flutter build linux --release`. The
Pixel was not attached: nothing installed. Box 7 of the hardware run now
checks two phones on `ws-opus`.

- 2026-11-05, the first video call from the Linux app to the phone on this
  build ended after 2 s as `error`, "its media went outside the tailnet":
  the Linux side's selected pair was `host/udp fd7a:115c:a1e0::3936:6f36
  to prflx/udp` with no address. libwebrtc's statistics hide a
  peer-reflexive remote's address, and the phone's end is always
  peer-reflexive (it signals no candidate), so the check failed every
  such call. `pathOnTailnet` (core policy) now passes a hidden
  peer-reflexive remote when our end is a tailnet address; protocol 4.11
  says so. Tests in `policy_test.dart` and `voice_calls_test.dart`.
