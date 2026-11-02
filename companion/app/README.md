# Thistle Chat, the companion app

Tailnet chat's companion app (`.scratch/tailnet-chat/spec.md`, ADR-0015) for
Linux and Android, from the same code. The protocol, the
policy, the outbox and the database are the Dart core's
(`companion/core/`, `tailnet_chat_core`); this app is the screens and the
platform glue:

- `lib/src/tailnet.dart`: `tailscale status --json` parsed (a pure function),
  whether Tailscale runs and why not, and discovery: each online tailnet
  peer's port 4810 probed with a hello.
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
  coming in rings with Answer and Decline buttons.
- `lib/src/talk.dart`: voice messages and calls: recording (two minutes at
  most) and playing, the call in progress, its ring and notifications,
  mute and volume. The codec, the Ogg/Opus files, the jitter buffer and a
  call's frames are the core's (`VoiceRecorder`, `decodeVoice`,
  `CallMedia`), Opus by FFI through the system's libopus on Linux and the
  app's own `libopus.so` on Android.
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
  listening).

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
flutter test                    # about 80 tests, 10 s
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
```

`--mic` is a 16 kHz mono 16-bit WAV file, played over and over as the
microphone (silence with `--speaker` alone); `--speaker DIR` writes each
thing played, a voice message, the ring or a call, to a numbered WAV file
there when it stops. Two instances on one machine can then call each other
without feedback, and what each heard can be checked.

### Echo cancellation

A call asks the platform to keep the speaker out of the microphone; the
call screen says which way it got:

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
- **A call coming in** in the background rings (the ringtone stream) and
  notifies on the `calls` channel, full screen where Android allows it;
  Answer and Decline both open the app, as the peer with the call lives in
  its window's engine.
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
  the contacts dialled on coming to the front, and the listening service
  started, switched off and on, with notifications asked for first.
- `test/ui_test.dart`: the same flows through the screens, by taps and
  typing; Android's banner for the Tailscale app, no discovery there, and a
  refused permission's banner; a voice message's player and the recording
  bar; the call screen placing, answering, muting and hanging up.
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
