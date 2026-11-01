# 11: Companion on Android

**What to build:** the companion app on an Android phone with the Tailscale app on. It is reachable in the background through a foreground service with a persistent notification, on by default and switchable; its identity key is in the Keystore; messages and calls notify in the background; chat peers are added by name or address, as Android has no peer list for it. Everything the Linux app does, the Android app does.

**Blocked by:** 06

**Status:** done but for the manual run on a phone (no phone was attached): the checklist is in the comments.

- [x] An APK builds from documented commands; the build is in CI.
- [ ] With the service on and the app swiped away, a message and a call from the Linux app arrive; with it off, the message arrives when the app is opened.
- [x] The app says when the Tailscale app is not connected.
- [x] Microphone and notification permissions are asked when first needed, and a refusal is explained.
- [ ] A manual run on a phone, written in the comments.

## Comments

Done on `tailnet-chat-11-companion-android`, but for the run on a phone.

- **The Android runner** (`companion/app/android/`, `flutter create --platforms=android`, application id `org.thistleos.thistle_chat`, label Thistle Chat). Core library desugaring is on (flutter_local_notifications needs it). The release APK is signed with the debug key, for installing by hand.
- **No new Dart packages.** The platform glue is the app's own method channel, `org.thistleos.thistle_chat/platform` (`Platform.kt`, `lib/src/android.dart`), so the Linux build gains nothing (no libsecret through `flutter_secure_storage`), and `pubspec.yaml`/`pubspec.lock` are untouched for ticket 10 to merge with.
- **Identity in the Keystore** (`KeystoreVault.kt`): the 64-hex seed sealed with AES-256-GCM under a non-exportable Keystore key (`thistle_chat_identity`), the sealed bytes in private preferences. Backup and device transfer are off (`allowBackup="false"`, `data_extraction_rules.xml`): a restored seed could not be unsealed. A seed that cannot be unsealed shows an error, as on Linux, and is never replaced.
- **The foreground service** (`ListenService.kt`, type `remoteMessaging`, `START_STICKY`), with a low-importance persistent notification that opens the app. It is on by default and follows Settings, Listen in the background (`ChatModel._applyBackground`, serialised, so a switch flipped during the notification prompt lands last). The listening chat peer is in the Dart isolate of one Flutter engine shared by the activity and the service (`Engines.kt`, `FlutterEngineCache`): swiping the app away destroys the activity but not the engine while the service runs; with the service off, the engine goes with the window. If Android kills the process, it restarts the service, which starts the engine again with no window, and `main()` sets the model to the background, so messages notify.
- **Tailscale app not connected**: `readAndroidTailnet` lists the network interfaces; the tailnet is a `tun*` interface with a 100.64.0.0/10 address (such an address on the mobile interface is carrier NAT, not the tailnet). Otherwise the banner says "The Tailscale app is not connected, so nothing can reach this phone. Open Tailscale and connect." `TailscaleStatus.hasPeerList` is false there: Contacts offers no "Look for chat peers", only Add by name or address (MagicDNS names resolve through the Tailscale app's DNS).
- **Without a peer list** the contacts known (their tailnet names) are dialled when the tailnet address appears or changes and each time the app comes to the front, so a message held by its sender arrives when the app is opened with the service off.
- **Permissions when first needed** (`permissions.dart`): notifications when the service starts, or a contact is added or a request accepted; the microphone is in the manifest and `model.permissions?.need(AppPermission.microphone)` is there for ticket 10 to call before recording or a call. Android before 13 has no notification prompt: a switch turned off in its settings counts as refused for good. A refusal shows a banner saying what it stops, with Open settings and Dismiss.
- **Tests**: 61 (`flutter test`, about 6 s; 20 new): Android's tailnet from interfaces, the permission flow, the service switch with a fake service, contacts dialled on coming to the front, and the screens (the Tailscale app banner, no discovery, the refusal banner). `flutter analyze --fatal-infos` clean; `flutter build linux --release` still builds.
- **CI**: a `companion-android` job in `tests.yml` (hosted runner's Android SDK and JDK 17, Flutter cloned at its tag): `flutter build apk --release`, the APK kept as the artifact `thistle-chat-apk` for 14 days. `tools/test_ci_workflows.py` passes.
- **This laptop**: the Android SDK licences were accepted (`flutter doctor --android-licenses`) and Flutter pointed at the SDK and JDK 21 (`flutter config --android-sdk ~/Android/Sdk --jdk-dir /usr/local/jdk-21.0.5+11`); the Gradle build installed platform 35 itself. `flutter build apk --release` builds a 59 MB APK (arm64, armv7, x86_64), with libsqlite3.so bundled by the core's sqlite3 hooks.

Remaining: the manual run on a phone with the Tailscale app, against the Linux app on another tailnet machine. To do, writing what happened here:

1. `adb install -r companion/app/build/app/outputs/flutter-apk/app-release.apk`; open Thistle Chat with the Tailscale app disconnected: the banner says the Tailscale app is not connected. Connect Tailscale: the banner goes within 30 s, and Settings says it listens on the phone's 100.x address, port 4810.
2. First start on Android 13 or later: the notification prompt shows once. Allow: the "Thistle Chat is listening" notification is there. (Second phone or after clearing data: refuse it; the banner explains, Open settings opens the app's settings page; the service still runs.)
3. Contacts: no "Look for chat peers"; Add by name with the laptop's MagicDNS name, then with its 100.x address: both resolve; send a message; the laptop shows a request; accept it there.
4. Swipe the app away from Recents, service on. From the Linux app send a text: a notification on the phone; tap it: the conversation opens with the message. Ring from the Linux app once ticket 10 has calls: a call notification.
5. Settings, Listen in the background off: the persistent notification goes. Swipe away; send from the Linux app (it waits there). Open the phone's app: the message arrives within a few seconds.
6. Restart the phone, open the app: the same fingerprint (Settings), read back from the Keystore.
7. Leave it in the background with the screen off for an hour, service on, then message it: does it arrive (Doze)? If not, note the phone and Android version; a battery-optimisation exemption prompt may be needed.
8. Microphone (with ticket 10): the first voice message or call asks; refusing shows the microphone banner.
