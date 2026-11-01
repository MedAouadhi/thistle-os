# Thistle Chat, the companion app

Tailnet chat's companion app (`.scratch/tailnet-chat/spec.md`, ADR-0015) for
Linux; Android comes in ticket 11 from the same code. The protocol, the
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
- `lib/src/notifier.dart`: desktop notifications (`flutter_local_notifications`,
  the freedesktop service over D-Bus).
- `lib/src/ui/`: Conversations, Contacts (discovery, add by name, a contact's
  fingerprint, a changed key to accept, rename, remove), Requests, Settings
  (read receipts, background listening).

## Building and running

Flutter 3.47.9 (Dart 3.13.7). The Linux build needs
`clang cmake ninja-build pkg-config libgtk-3-dev`; nothing else, no
libsecret.

```sh
export PATH=$HOME/flutter/bin:$PATH
cd companion/app
flutter pub get --enforce-lockfile
flutter analyze
flutter test                    # about 40 tests, 10 s
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
```

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

## Tests

- `test/tailnet_test.dart`: `tailscale status --json` fixtures
  (`test/fixtures/`, made up) running, stopped and signed out; discovery.
- `test/identity_test.dart`: the key made once and kept; the real Secret
  Service when a desktop session bus is there (skipped in CI).
- `test/chat_model_test.dart`: the model against a real core peer on
  loopback: requests, accept and decline, delivered and read, replies,
  reactions, typing, notifications, read receipts off, rename, remove, a
  changed key flagged and accepted, discovery, a peer by name.
- `test/ui_test.dart`: the same flows through the screens, by taps and
  typing.
