# 11: Companion on Android

**What to build:** the companion app on an Android phone with the Tailscale app on. It is reachable in the background through a foreground service with a persistent notification, on by default and switchable; its identity key is in the Keystore; messages and calls notify in the background; chat peers are added by name or address, as Android has no peer list for it. Everything the Linux app does, the Android app does.

**Blocked by:** 06

**Status:** ready-for-agent

- [ ] An APK builds from documented commands; the build is in CI.
- [ ] With the service on and the app swiped away, a message and a call from the Linux app arrive; with it off, the message arrives when the app is opened.
- [ ] The app says when the Tailscale app is not connected.
- [ ] Microphone and notification permissions are asked when first needed, and a refusal is explained.
- [ ] A manual run on a phone, written in the comments.
