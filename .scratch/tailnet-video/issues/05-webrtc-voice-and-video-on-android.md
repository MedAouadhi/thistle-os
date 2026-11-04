# 05: WebRTC voice and video on Android

**What to build:** the Android companion app announces `call:webrtc` and calls the Linux app and other Android apps on WebRTC, voice and video, as ticket 04 does on Linux; calls with the T-Deck stay `ws-opus`. The camera is asked for the first time it is switched on, and a refusal sends No camera while the call goes on. Leaving the app pauses the camera (the other side sees Camera paused) and keeps the voice in the listening service; returning resumes it. While either side's video is on screen, audio plays on the loudspeaker with the screen kept on and no proximity screen-off; with both cameras off, the earpiece and the proximity lock come back. Android starts on the front camera, with Switch camera. The lock-screen call window from ticket 01 offers Answer with video when the offer has `video`. The Android artifacts are pinned by Gradle's dependency verification. See `../spec.md`, user stories 24 to 28 and 43.

**Blocked by:** 01, 04

**Status:** ready-for-agent

- [ ] The call media engine runs on Android; a call between the Android app and the Linux app is WebRTC, voice and video both ways
- [ ] Camera permission asked on first use; refused, `call_camera` `none` and the call continues
- [ ] Leaving the activity sends `paused` and keeps audio; returning resumes the camera and sends `on`; no picture-in-picture, no camera foreground-service type
- [ ] The audio route follows the video: loudspeaker and screen kept on while any video shows, earpiece and proximity lock otherwise
- [ ] Front camera first; Switch camera to the back one
- [ ] The lock-screen call window shows Answer with video for a `video` offer
- [ ] Gradle dependency verification covers flutter_webrtc's Android artifacts; CI's Android build passes with it
- [ ] The app's call tests, with the fake media engine: a refused permission sends `none`, leaving the app sends `paused`, the audio route follows the video
