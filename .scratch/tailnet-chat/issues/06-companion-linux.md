# 06: Companion app on Linux, talking to the simulator and to itself

**What to build:** the Flutter app on Linux over the core: Conversations, Contacts and Requests, the fingerprint view, settings (read receipts, background listening). It finds chat peers from `tailscale status --json` by probing each online tailnet peer's port, and takes a name or address by hand. Its identity key is kept in the Secret Service. Messages notify when it is in the background. It tells the user when Tailscale is not running. It chats with the simulator, and two instances on two machines of the tailnet chat with each other.

**Blocked by:** 05

**Status:** done but for the manual run: a chat with the simulator (its chat peer is on `tailnet-chat` now) and with a second instance on another tailnet machine (below).

- [x] Builds and runs on Linux from documented commands; the build is in CI.
- [x] Discovery lists the chat peers among the online tailnet peers; a peer not discovered can be added by name or address.
- [x] The identity key survives a restart and is not in a plain file.
- [x] Every feature of tickets 03 and 04 is usable from the UI: requests, accept and decline, key change warning and acceptance, rename, remove, text, replies, reactions, typing, receipts.
- [ ] A manual run, written in the comments: the app chats with the simulator, and with a second instance on another tailnet machine.

## Comments

Done on `tailnet-chat-06-companion-linux`.

- `companion/app/` (`thistle_chat`, Flutter 3.47.9, Linux only for now; `companion/app/README.md`). The protocol, policy, outbox and storage stay in the core; the app is `tailnet.dart` (`parseTailscaleStatus`, a pure function, and `discoverChatPeers`), `identity.dart` (the `KeyVault` seam and the Secret Service), `chat_model.dart` (a `ChangeNotifier` over `ChatPeer`), `notifier.dart` and the screens under `ui/`.
- **The Secret Service is spoken to over D-Bus** (`dbus` 0.8.0, pure Dart), not through `flutter_secure_storage`, whose Linux side links libsecret and jsoncpp: the build needs only GTK, and this laptop has no `libsecret-1-dev`. The item is in the default collection, attributes `application org.thistleos.thistle_chat` and `profile`; without a Secret Service the app shows an error and does not fall back to a file.
- **Listening** is on the tailnet IPv4 address from `tailscale status --json`, re-read every 30 s (a change of address restarts the listener; each read calls `seenOnline` with the online peers' hostnames and MagicDNS names). Stopped, signed out, not running and not installed each get their own banner text.
- **Notifications** (`flutter_local_notifications` 22.3.3, D-Bus on Linux) for a message, a request and a ringing call while the window is not in front; none for the conversation on screen, which is marked read instead.
- **Background listening** is stored (`background_listening` in the core's settings) and shown; on Linux it changes nothing, the app listening while it runs. Ticket 11's foreground service reads it.
- **Core fix**: frames still in flight on a connection the core had closed were acted on, so a peer refused for a changed key had its message held as a contact request from the new key (seen as a flaky app test). The core now drops them (`peer.dart`, `_run`); regression test in `peer_test.dart`.
- Tests: 41 (`flutter test`, about 10 s): status fixtures (made up; running, stopped, signed out), discovery, the vault, the options, the model and the screens against a real core peer on loopback. The real-Secret-Service test runs only with a desktop session bus (skipped in CI). Core: 41.
- CI: a `companion-app` job in `tests.yml` (hosted; Flutter cloned at its tag, no third-party action): `pub get --enforce-lockfile`, format, `analyze --fatal-infos`, test, `build linux --release`. `tools/test_ci_workflows.py` passes.

Manual run, 2026-11-02, on this laptop (on the tailnet as 100.91.111.54):

- `flutter build linux --release` builds; the bundle links no libsecret.
- The release bundle under Xvfb, `--profile manual06 --verbose`, listened on `100.91.111.54:4810`. The reference peer (`peer.py --listen 100.91.111.54:4811`) added it by address and sent a text: the app logged the connection (`stranger`) and the request, the peer's message went to `delivered`, the peer learnt the app's tailnet name (`mohamed-Precision-7560`), and the screenshot showed Requests with a badge of 1. Restarted, it listened with the same fingerprint, read back from the keyring. The keyring item and database were deleted afterwards.
- Discovery against the real tailnet: Running, 3 online peers, none a chat peer yet (the device's side is ticket 03).
- No input could be sent to the Xvfb window, so accepting and chatting by hand were not done in the built app; the widget tests do those flows through the screens.

Remaining (manual): the app chatting with the simulator once ticket 03 is merged (`thistle_chat --listen 127.0.0.1 --loopback --discovery-port <sim port>` or the simulator on the tailnet), and two instances on two tailnet machines chatting both ways (requests, receipts, typing, replies, reactions, a key change after deleting one's keyring item).
