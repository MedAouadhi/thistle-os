# Thistle Chat, the companion app

Tailnet chat's companion app (`.scratch/tailnet-chat/spec.md`, ADR-0015) for
Linux and Android, from the same code. The protocol, the
policy, the outbox and the database are the Dart core's
(`companion/core/`, `tailnet_chat_core`); this app is the screens and the
platform glue:

- `lib/src/tailnet.dart`: `tailscale status --json` parsed (a pure function),
  whether Tailscale runs and why not, and discovery: each online tailnet
  peer's port 4810 probed with a hello, or on Android each tailnet address
  the contacts share (`discoverThroughContacts`).
- `lib/src/identity.dart`: the identity key's seed, made on first start and
  kept in the Secret Service (GNOME Keyring, KWallet), spoken to over D-Bus,
  so the build needs no libsecret. Never in a plain file: without a Secret
  Service the app says so and stops.
- `lib/src/chat_model.dart`: the app's state over the core's `ChatPeer`:
  where it listens (the tailnet address, once Tailscale runs, re-read every
  30 s), discovery, a peer added by name or address, the conversation on
  screen (read when in front), typing, notifications in the background, and
  the settings.
- `lib/src/notifier.dart`: notifications (`flutter_local_notifications`: the
  freedesktop service over D-Bus, Android's notification manager); a call
  coming in rings with Answer and Decline buttons, and on Android the phone
  rings itself (`CallRinger`, below).
- `lib/src/talk.dart`: voice messages and calls: recording (two minutes at
  most) and playing, the call in progress, its ring and notifications,
  mute and volume. The codec, the Ogg/Opus files, the jitter buffer and a
  call's frames are the core's (`VoiceRecorder`, `decodeVoice`,
  `CallMedia`), Opus by FFI through the system's libopus on Linux and the
  app's own `libopus.so` on Android. A call with another companion app
  runs on WebRTC instead (below), its media the call engine's.
- `lib/src/call_engine.dart`: the call media engine, the seam to WebRTC:
  start a call's media, take the other side's SDP and candidates, mute,
  volume, the cameras and the one sending, each side's video to draw, the
  candidate pair in use, hang up. `lib/src/webrtc_engine.dart`
  implements it over flutter_webrtc; the tests use a fake
  (`FakeCallEngine` in `test/helpers.dart`).
- `lib/src/audio/`: the microphone and the speaker: on Linux PulseAudio's
  simple API by FFI (`pulse.dart`; PipeWire answers it through
  pipewire-pulse), each stream in an isolate of its own; on Android
  AudioRecord and AudioTrack over the app's audio channels
  (`android_audio.dart`, `Audio.kt`); or WAV files (`--mic`, `--speaker`).
- Android only: `lib/src/android.dart` over the app's own method channel
  (`android/app/src/main/kotlin/org/thistleos/thistle_chat/`): the identity's
  seed sealed by an AES-GCM key in the Android Keystore (`KeystoreVault.kt`;
  no plugin, so the Linux build stays free of libsecret); the listening
  service (`ListenService.kt`, `background.dart`); runtime permissions
  (`Platform.kt`, `permissions.dart`). The tailnet is read from the network
  interfaces (`readAndroidTailnet` in `tailnet.dart`): the Tailscale app
  connected is a `tun` interface with a 100.64.0.0/10 address.
- `lib/src/ui/`: Conversations (with a voice message's player, the record
  button and the call button), the call screen (`call_page.dart`), Contacts
  (discovery, add by name, a contact's fingerprint, a changed key to
  accept, rename, remove), Requests, Settings (read receipts, background
  listening). The look is `theme.dart`'s, ink on paper as the T-Deck's
  e-paper is: warm paper, ink, hairlines and one signal colour, Fraunces,
  Instrument Sans and JetBrains Mono bundled in `assets/fonts/` (SIL OFL).
  `widgets.dart` has the pieces the pages share: a contact's monogram on
  its key's tint, the paper's grain, the voice waveform. What moves for its
  own sake (an online dot breathing, a call's ripples) stops when the
  platform asks for less motion, and under `flutter test`. A phone has no
  tabs: its home is the chats, with your monogram for Settings, the
  requests waiting as a row over them and New chat for the contacts, each
  a page of its own. A window 600 px wide gets the sidebar instead, and one
  1100 px wide a conversation beside the list.

## Building and running

Flutter 3.47.9 (Dart 3.13.7). The Linux build needs
`clang cmake ninja-build pkg-config libgtk-3-dev`; nothing else, no
libsecret. Voice and calls load `libopus.so.0` and `libpulse-simple.so.0`
when first used (`sudo apt install libopus0 libpulse0`; both come with any
desktop); without them the app works and says why it has no audio.

```sh
export PATH=$HOME/flutter/bin:$PATH
cd companion/app
flutter pub get --enforce-lockfile
flutter analyze
flutter test                    # about 110 tests, 15 s
flutter build linux --release   # build/linux/x64/release/bundle/thistle_chat
flutter run -d linux            # or: run the bundle
```

The app listens on this machine's tailnet address, port 4810, and says so
in Settings. With Tailscale stopped, signed out or not installed, a banner
says which, and nothing listens until it runs.

Its options, for a second instance on one machine or a peer on loopback:

```sh
thistle_chat --profile second --port 4811        # its own key, database and port
thistle_chat --listen 127.0.0.1 --loopback       # chat with the simulator on this machine
thistle_chat --discovery-port 4811 --verbose     # probe another port; log the core's events
thistle_chat --mic tone.wav --speaker heard/     # audio files instead of a sound card
thistle_chat --no-webrtc                         # every call on ws-opus, as before WebRTC
```

`--mic` is a 16 kHz mono 16-bit WAV file, played over and over as the
microphone (silence with `--speaker` alone); `--speaker DIR` writes each
thing played, a voice message, the ring or a call, to a numbered WAV file
there when it stops. Two instances on one machine can then call each other
without feedback, and what each heard can be checked.

### Calls between companion apps: WebRTC

On Linux the app announces `call:webrtc` (protocol section 4.11, ADR-0016),
and a call with another companion app that does runs on WebRTC through
flutter_webrtc and its prebuilt libwebrtc, camera or not; a call with the
T-Deck, or with an app that does not announce it (Android until ticket 05
of `.scratch/tailnet-video`, or `--no-webrtc`), stays `ws-opus`. The
caller makes the SDP offer; one audio and one video track are negotiated
from the start, so a camera switched on, off or to another replaces the
track and never renegotiates. There is no STUN or
TURN server and no TCP candidate; the core sends and takes only UDP
candidates in the tailnet's ranges, and the app reads the pair in use
from WebRTC's statistics 2 s after it connects and every 5 s after,
ending the call as an error if either end is outside the tailnet. A call
whose media does not connect within 15 s of the answer ends as "it could
not connect"; one disconnected for 10 s, as "the connection dropped".
With `--verbose` the log says the transport, each candidate's kind and
whether it went, the pair in use, the cameras found, each camera change
and the size it captures, the video's frames sent and received at the
end, and why the call ended.

### Video

A conversation with a contact whose app announced `call:webrtc` has a
Video call button beside Call; the T-Deck and older apps have none
(`ChatPeer.videoCapable`, from the contact's last hello this run, so the
button shows once the app has been connected to it). Video call offers
with `video` and our camera on from the answer. A call offered with
`video` rings as "Video call", in the notification too, with Answer with
video, Answer (the camera stays off) and Decline; missed, it is "Missed
video call". The camera is never on without a tap.

During a WebRTC call the call screen has the camera switch, and Switch
camera while the camera is on and the machine has more than one (in the
order flutter_webrtc lists them, the first to start with). The other
side's video fills the screen while its `call_camera` is `on`; otherwise
a tile says Camera off, Camera paused or No camera. Ours is in the top
right corner. A machine with no camera says `none`, its switch says No
camera, and it still sees the other side. The camera is asked for at
most 1280x720 at 30 fps, and at most 30 fps are sent; WebRTC's
congestion control lowers both to fit the path.

A camera that opens and gives no frame within 2.5 s (flutter_webrtc
issue 2225: another process holds it) is turned off, the other side told
`off`, and the call screen says so. libwebrtc opens the camera without
close-on-exec, so a child process started while it is open keeps it and
the next call's camera gives nothing: `lib/src/child_processes.dart`
starts no child (`tailscale status`, PipeWire's echo canceller) while a
camera is on, and opens a camera only once those running have exited.
Tailscale's status is not read again meanwhile.

The prebuilt libwebrtc (24 MB, in the bundle's `lib/`) is fetched by the
Linux build at the release `linux/libwebrtc.cmake` names and checked
against the SHA-256 there before flutter_webrtc's own build looks for it;
another hash fails the build. Moving flutter_webrtc to a release that asks
for another libwebrtc fails there too until the pin follows it.

`tool/webrtc_call_check.dart` calls between two of the app's models in
one process with the real libwebrtc, twice, over this machine's tailnet
address, then makes a video call on this machine's camera (the caller's
camera on, off, then the callee's, never both: one camera opens once),
reading each side's frames and their size from WebRTC's statistics, and
prints PASS or FAIL; it needs Tailscale up, a sound server, a camera and
a display, so CI does not run it. Built in place of the app (build
the app again after):

```sh
flutter build linux --release -t tool/webrtc_call_check.dart --dart-define=TAILNET_IP=$(tailscale ip -4)
xvfb-run -a build/linux/x64/release/bundle/thistle_chat
```

### Echo cancellation

A WebRTC call uses libwebrtc's own echo canceller, in the app; no PipeWire
canceller is started for it, and the call screen says "on (WebRTC's own
canceller, in the app)". A `ws-opus` call asks the platform to keep the
speaker out of the microphone; the call screen says which way it got:

1. **PipeWire** (Ubuntu 22.10 and later, Fedora): for the length of a call
   the app runs `pipewire -c echo-cancel.conf`, a client of the running
   PipeWire that loads `libpipewire-module-echo-cancel` with WebRTC's echo
   canceller (`aec/libspa-aec-webrtc`, in the `pipewire` package) and makes
   two nodes, `thistle-chat-ec-source` and `thistle-chat-ec-sink`. The call
   records from the one and plays to the other, and the canceller is
   stopped when the call ends. The file is written to the profile's data
   folder from `echoCancelConf` in `lib/src/audio/devices.dart`; nothing in
   the user's PipeWire configuration changes.
2. **PulseAudio**: the call's streams carry `filter.want=echo-cancel` and
   `media.role=phone`, which PulseAudio's `module-filter-apply` (loaded by
   default) answers with `module-echo-cancel`.

Neither can be told from here to have worked on PulseAudio; with neither,
a headset avoids the echo.

`--profile` keeps a separate Secret Service item (attributes `application
org.thistleos.thistle_chat`, `profile NAME`) and database
(`~/.local/share/org.thistleos.thistle_chat/NAME/chat.db`).

To chat with the reference peer on the tailnet address:

```sh
python3 tools/tailnet_chat/peer.py --listen 100.x.y.z:4811 --name reference
```

and type on its standard input, a command a line (the control protocol in
`docs/tailnet-chat/README.md`):

```json
{"cmd": "add_contact", "address": ["100.x.y.z", 4810], "name": "laptop", "seq": 1}
{"cmd": "send_text", "to": "laptop", "text": "hello", "seq": 2}
```

It shows in Requests with its fingerprint; Accept, and the conversation
opens in Conversations.

## Android

The Tailscale app must be installed and connected on the phone: the chat
app has no peer list there, so chat peers are added by tailnet name
(MagicDNS) or 100.x address, and the contacts known are dialled when the
tailnet comes up and whenever the app comes to the front.

**Look for chat peers** asks each contact that announces `peers` (a
laptop's app, the T-Deck) for the online peers its platform lists
(protocol section 4.10), dialling it if need be, a few seconds each; probes
each address they share on the discovery port with a hello; and lists the
chat peers that answer, each "via" the contact that shared it. Contacts
and this phone are left out. Only addresses inside the tailnet's ranges are
taken from a shared list, never a name or a key: Add pins the key at the
first handshake, as for an address typed in. On Linux the app answers a
contact that asks with what `tailscale status --json` shows online.

- **Listening in the background**: a foreground service with a persistent
  notification ("Thistle Chat is listening") keeps the process, and the
  Flutter engine whose chat peer listens, alive after the app is swiped
  away. On by default; Settings, Listen in the background, switches it.
  Off, a message waits with its sender until the app is opened. The engine
  is the activity's and the service's both (`Engines.kt`): the window
  attaches to it, and Android restarting the service restarts it.
- **Permissions, when first needed**: notifications when the service starts
  or a contact is added or accepted, the microphone when a recording or a
  call first needs it (`model.permissions?.need(AppPermission.microphone)`).
  A refusal shows a banner saying what it stops, with Open settings.
- **Voice messages and calls**: Opus is the repository's own libopus
  (`components/opus/`, 1.5.2), built for each ABI by the app's CMake
  (`android/app/src/main/cpp/CMakeLists.txt`) as `libopus.so`, which the
  core opens. A voice message records from the microphone and plays as
  media. A call puts the phone in communication mode, records from the
  voice-communication source with Android's AcousticEchoCanceler (and
  NoiseSuppressor) where the phone has them, which the call screen says,
  plays on the voice-call stream (the earpiece, unless a headset or the
  phone's own routing says otherwise), and darkens the screen at the ear.
  While a call is up the listening service is in the foreground as using
  the microphone too (`remoteMessaging|microphone`), started from the call
  screen, so the call keeps its audio with the app in the background;
  Android 14 allows that type to start only with the app in front. With
  listening switched off, the service runs for the call alone.
- **A call coming in rings like a phone call** (`CallRing.kt`), whoever
  calls: the phone's default ringtone, looping on the ringtone stream, with
  a vibration a second on and a second off, until the answer, the decline,
  the caller's end or 45 s. Both follow the ringer mode (vibrate: no sound;
  silent: neither; ring: vibration as "vibrate when ringing" says) and Do
  Not Disturb, which lets a call through only when it allows calls from
  anyone. The app's own two-tone ring is Linux's alone. With the app out of
  sight it also shows Android's call-style notification with Answer and
  Decline, silent itself, on the `incoming_calls` channel (the old `calls`
  channel had a sound of its own, which Android keeps, and is deleted).
- **The call window** (`CallActivity.kt`), which that notification opens
  full screen: over the lock screen, turning the screen on, in a task of
  its own, on the same engine. While it is attached the app draws the call
  screen and nothing else (`CallWindowed` in `call_page.dart`); answering
  needs no unlock, and when the call screen closes the window finishes,
  so a locked phone is back at its lock screen. The main window stays off
  the lock screen, and makes itself anew when it next shows, as the call
  window took the engine from it. The notification's Answer opens the
  window and answers; its Decline answers through a receiver, no window.
- **Calls filling the screen**: Android 14 and later allow it only to
  calling apps unless the user allows it. The app checks at start, when a
  call comes and when it comes to the front; without it Settings says
  "Calls can't fill the screen", with Allow opening Android's page, and a
  call rings as its notification alone.
- Nothing is backed up (`allowBackup="false"`, `data_extraction_rules.xml`):
  the sealed seed is useless without this phone's Keystore key.

Building needs the Android SDK (platform 35 and 36, build tools, the NDK
Flutter names and CMake 3.22.1 for libopus, all installed by the Gradle
build once the licences are accepted; `sdkmanager --install
"ndk;28.2.13676358" "cmake;3.22.1"` beforehand) and JDK 17 or newer:

```sh
export PATH=$HOME/flutter/bin:$PATH
flutter config --android-sdk ~/Android/Sdk --jdk-dir /path/to/jdk-17-or-21
flutter doctor --android-licenses
cd companion/app
flutter build apk --release     # build/app/outputs/flutter-apk/app-release.apk
adb install -r build/app/outputs/flutter-apk/app-release.apk
```

The APK is signed with the debug key, for installing by hand; CI keeps the
one it builds as the artifact `thistle-chat-apk`.

## Tests

- `test/tailnet_test.dart`: `tailscale status --json` fixtures
  (`test/fixtures/`, made up) running, stopped and signed out; discovery.
- `test/android_tailnet_test.dart`: Android's tailnet from the interfaces:
  a `tun` with a 100.64.0.0/10 address, carrier NAT on the mobile interface
  not taken for it, the Tailscale app not connected.
- `test/permissions_test.dart`: asked when first needed, once; a refusal
  explained; refused for good sends to the settings.
- `test/identity_test.dart`: the key made once and kept; the real Secret
  Service when a desktop session bus is there (skipped in CI).
- `test/chat_model_test.dart`: the model against a real core peer on
  loopback: requests, accept and decline, delivered and read, replies,
  reactions, typing, notifications, read receipts off, rename, remove, a
  changed key flagged and accepted, discovery, a peer by name; on Android,
  discovery through a contact's shared list, the contacts dialled on coming
  to the front, and the listening service
  started, switched off and on, with notifications asked for first.
- `test/ui_test.dart`: the same flows through the screens, by taps and
  typing; Android's banner for the Tailscale app, discovery through the
  contacts there, and a
  refused permission's banner; a voice message's player and the recording
  bar; the call screen placing, answering, muting and hanging up; video:
  the Video call button by capability, the ring screen's three buttons
  and the plain Answer, the camera switch, Switch camera with one and two
  cameras, no camera, and the other side's tile for each camera state.
- `test/voice_calls_test.dart`, WebRTC: two models calling with the fake
  call engine, the offer and answer and the tailnet's candidate alone
  crossing, no PipeWire canceller, the camera said off; mute, volume and
  hang up reaching the engine; the T-Deck still `ws-opus`; a pair outside
  the tailnet, media that never connects, and media lost, each ending the
  call; video: `video` on the offer and the caller's camera on, Answer
  and Answer with video, the switch and Switch camera, no camera sending
  `none`, a camera that gives no picture, the video call's notifications,
  and no `tailscale status` while a camera is on.
- `test/child_processes_test.dart`: no child starts while a camera is on,
  and a camera waits for those running.
- `test/voice_calls_test.dart`: voice messages recorded from a (fake)
  microphone reach a core peer as Ogg/Opus of their length, decoding to the
  tone recorded, and received ones play whole only when asked; calls placed
  and answered carry the microphone both ways, mute sends silence, a call
  in the background rings with a notification, decline, a missed call,
  unreachable; a refused microphone on Android.
- `test/android_audio_test.dart`: the Android audio devices against a fake
  of `Audio.kt`'s channels: a recording's frames, a refused microphone, a
  voice message's PCM to the speaker and drained, the ring, a call's
  begin, echo canceller and end (and a phone with none), a call whose
  microphone or speaker will not open undone.
