# 10: Companion: voice messages and live calls

**What to build:** the companion app records, sends, receives and plays voice messages, and places and takes live `ws-opus` calls, with the platform's echo cancellation on, on Linux. It does both with the simulator, with a T-Deck, and with another companion app.

**Blocked by:** 06

**Status:** done but for the manual runs that need the simulator's voice and calls (tickets 04 and 09) and a T-Deck: the Linux-to-Linux run is in the comments.

- [x] Voice messages are Ogg/Opus at 16 kHz mono, up to 2 minutes, as the protocol's vector.
- [x] Calls ring with a notification when the app is in the background; answer, decline, hang up and mute work.
- [x] The conformance suite's voice and call scenarios pass against the headless core.
- [ ] A manual run, written in the comments: a voice message and a call each way with the simulator, and a call between two Linux instances. (Two Linux instances: done. The simulator: waits for 04 and 09.)

## Comments

Done on `tailnet-chat-10-companion-voice-calls`.

### The core (`companion/core/`, pure Dart)

- **Opus by FFI to the system's libopus** (`opus.dart`; `libopus.so.0`, Debian/Ubuntu `libopus0`, which every desktop has and CI installs). No pub package: the maintained Flutter Opus packages bundle their own native build per platform, and the core must stay pure Dart for `dart run`. `ffi` 2.2.0 was already locked as a transitive dependency and is now direct. `OpusEncoder` (16 kHz, VOIP, 24 kbit/s, in-band FEC), `OpusDecoder` (null conceals a lost packet), `VoiceRecorder` (PCM in any chunks; stops taking audio at 2:00; flushes the encoder's lookahead with one frame of silence so the end is not cut) and `decodeVoice` (pre-skip dropped, the end trimmed to the length). The encoder's lookahead is the vector's pre-skip, 312.
- **Ogg/Opus** (`ogg_opus.dart`): read with every page's CRC checked, and written one second a page with the last page's granule ending the audio at its length (as FFmpeg does for the vector: 76 packets, granule 72312, 1500 ms). Tested against `voice.opus`/`voice.json` and against the reference peer's Python reader.
- **Jitter buffer** (`jitter.dart`, pure): holds 60 ms before playing, plays in sequence, conceals a missing packet once, drops a late one, fills again after running dry holding 20 ms more each time up to 200 ms, cuts more than 200 ms queued back to the target, follows a sender that starts counting again.
- **Call audio** (`call_media.dart`): `CallMedia(peer, call, mic:, speaker:)` encodes the microphone into `ws-opus` frames and plays the other side's through the jitter buffer and the decoder, paced by the clock (a late timer catches up); mute sends silence, so frames keep going and the far end does not time out; volume 0 to 2; stops by itself when the call ends. `ChatPeer.callAudio` is the new hook for incoming frames (a `Stream<CallAudio>`).
- **Call state** (`policy.dart`, pure): `callEndDue` (10 s without a frame, and **45 s of ringing unanswered**, on either side, end as `timeout`; the protocol has no ring limit, this is ours) and `isMissedCall`. `Call` gained `since`, `answered`, `missed`. A call whose connection drops now ends at once as `ended:error` (it waited out the 10 s before); `endCall` on a call already over sends nothing; a stray `call_answer` for a call no longer offered is ignored.
- **Headless core**: a connected call streams `--mic FILE.wav` (or silence) and writes what it hears to `--speaker FILE.wav`; control commands `answer_call`, `talk` (a no-op: it streams by itself), and `calls` reports `mine` and `missed`. `wav.dart` reads and writes 16 kHz mono WAV.
- Tests: 76 (`dart test`, about 7 s), from 41.

### The conformance suite

A block of its own in `tools/tailnet_chat/conformance.py`, `VOICE_SCENARIOS` and `CALL_SCENARIOS`, added to `SCENARIOS`: `voice_message_from_target` (the target's voice message as `voice` and chunks of the whole file; delivered on the ack), `voice_message_broken_and_restarted` (closed after the first chunk, sent again whole on the next connection), `call_answered` (rings; the user answers; audio both ways on the offer's stream, in order; hung up by the other side), `call_placed_by_target`, `call_declined` (both ways), `call_busy` (and a stranger's call `declined`), `call_ended_by_cellular`, `call_far_end_silent` (`call_end timeout` 9.5 to 13.5 s after the last frame), `call_unreachable` (fails within 8 s, nothing queued). The existing `voice_message_whole_and_broken` covers receiving.

New `Target` methods: `send_voice`, `set_call_policy`, `place_call`, `calls`, `answer_call`, `end_call`, `talk`, `cellular_call`, and `can_cellular`; `Skipped` is reported as `skip`, not a failure (`run(..., skipped=)`). A simulator target (ticket 09) answers `set_call_policy("ignore")` by doing nothing, since its calls always ring, `talk` by doing nothing (its microphone streams), and `cellular_call` with the fake modem's `--call`.

The reference peer now sends `call_end timeout` on its 10 s timeout (`call_silence_s`, settable for tests), has `cellular_call()` and `talk()`, and its control protocol `answer_call`, `talk`, `cellular_call`.

Results: 25 of 25 against the reference peer in-process and as a process; 24 of 25 against the headless core with `call_ended_by_cellular` skipped. `tools/test_tailnet_chat.py` 44 tests (it runs every scenario against the reference peer both ways); `tools/test_ci_workflows.py` passes. The suite now takes about 17 s.

### The app (`companion/app/`)

- `lib/src/audio/pulse.dart`: the microphone and the speaker through PulseAudio's simple API by FFI (`libpulse-simple.so.0`, which PipeWire answers through pipewire-pulse), each stream in an isolate of its own since its reads and writes block; 20 ms fragments for capture, 60 ms target for playback. No audio plugin: the ones on pub.dev for Linux go through GStreamer or need extra `-dev` packages at build time, and none offers echo cancellation.
- **Echo cancellation** (`lib/src/audio/devices.dart`, README "Echo cancellation"): on PipeWire, for the length of each call the app runs `pipewire -c echo-cancel.conf`, a client of the running PipeWire that loads `libpipewire-module-echo-cancel` with WebRTC's canceller and makes `thistle-chat-ec-source`/`-sink`; the call records from the one and plays to the other, and the process is killed when the call ends. Nothing in the user's configuration changes. On PulseAudio the streams carry `filter.want=echo-cancel media.role=phone`, which the default `module-filter-apply` answers. The call screen says which applied.
- `lib/src/talk.dart` (`model.talk`): recording (asks for the microphone first through `model.permissions`, null on Linux), playing (only when asked; Stop stops at once), the call in progress, the ring (a two-tone ring through the speaker while a call rings), notifications (a ringing one with Answer and Decline buttons, cancelled once it stops ringing; "Missed call from X"), mute and volume. The app's core peer now uses call policy `ignore`: every call rings for the user.
- Screens: the conversation has a call button, a record button that turns the compose row into a recording bar (time of 2:00, throw away, send), and voice bubbles with play/stop, progress and length; the call screen (`ui/call_page.dart`) shows calling, ringing (Answer, Decline), the time, the echo cancellation, mute, volume and hang up, then the outcome ("Not reachable now. Leave a voice message instead.", Busy, Declined, no sound for 10 s, the connection dropped) and closes after 3 s. A call coming in opens the call screen over whatever is in front.
- `--mic FILE.wav` and `--speaker DIR`: audio files instead of a sound card (`FileAudioDevices`), for two instances on one machine and for this ticket's manual run.
- Android: `NoAudioDevices` ("voice and calls are not on Android yet"): it needs libopus built for Android and a device layer (AAudio or a platform channel to `AudioRecord`/`AudioTrack` with `AcousticEchoCanceler`), and the foreground service declaring microphone use during a call. Left to a later ticket; `flutter build apk` still builds.
- Tests: 75 (`flutter test`, about 7 s), from 61 after the merge with 11: `voice_calls_test.dart` (the model against a core peer with fake devices: a recording reaches it as Ogg/Opus of its length decoding to the tone, a cancelled one sends nothing, a received one plays whole only when asked, a refused microphone; calls placed and answered carrying the tone both ways, mute, background ring with a notification, decline, missed, unreachable) and three screen tests in `ui_test.dart`.
- CI: `libopus0` for the core's job; `libopus0 libpulse0` added to the app's apt line.

### Manual run on this laptop (2026-11-02)

Two release bundles under Xvfb (`:77` alpha, `:78` beta, 1400x800), driven by XTest through ctypes (no xdotool here); Secret Service profiles `m10a`/`m10b`, listening on 127.0.0.1:4831/4832 with `--loopback`. Script and screenshots in the session's scratchpad (`manual10/`).

1. Files: alpha `--mic tone440.wav --speaker outA`, beta `--mic tone1000.wav --speaker outB`. Beta added `127.0.0.1:4831` by address, recorded a voice message (the bar showed "Recording 0:02 of 2:00") and sent it: Delivered. Alpha saw a request "Voice message", accepted, played it (bubble "0:01 / 0:02", progress bar): `outA/001-played.wav`, 2.68 s of 1000 Hz at a steady level. Alpha recorded one back: beta's bubble went to Read.
2. Beta called: alpha's call screen came up over the conversation, "beta, Calling you", Decline and Answer (and `002-ring.wav`, the ring, was written). Answered: both showed the time and "Echo cancellation: none (audio files)". Alpha heard 1000 Hz for the whole call and beta 440 Hz (16 s each, level steady within 0.3 %); alpha's mute took beta's side to silence (rms 0) from that moment; beta hung up and both showed "Call ended".
3. Alpha on the real sound server (PipeWire 1.0.5 through pipewire-pulse), beta on a quiet 1000 Hz file: alpha's call screen said "Echo cancellation: on (PipeWire, WebRTC echo canceller)"; the frames flowed both ways (beta wrote 5.3 s; this laptop's microphone gave silence, as it did to a direct capture, so it is muted or absent); after the hang-up no `thistle-chat-ec` node was left in `pw-cli ls Node`.

Not checked here: what the freedesktop notification looks like and its Answer/Decline buttons (Xvfb has no notification service; the model's side is tested), and how well the echo is cancelled with a real speaker and microphone.

### Left for later runs

- **The simulator** (needs 04's messages and 09's calls in the simulator): a voice message each way between the app and the simulator, and a call each way, with `thistle_chat --listen 127.0.0.1 --loopback` and the simulator's `THISTLE_SIM_MIC` and speaker file; point `conformance.py`'s voice and call scenarios at the simulator target.
- **A T-Deck**: a call each way over WiFi and over LTE through DERP, with the laptop's speaker and microphone (not a headset), to judge the echo cancellation and the delay; a voice message each way.
- **A second machine**: two Linux instances on different laptops over the tailnet with real audio, each way.
