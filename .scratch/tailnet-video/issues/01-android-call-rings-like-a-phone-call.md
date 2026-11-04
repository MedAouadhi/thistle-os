# 01: Android: an incoming call rings like a phone call

**What to build:** a call to the Android companion app rings like a phone call, whoever calls (a companion app or the T-Deck) and on today's `ws-opus` calls. On a locked phone the screen turns on and a call window shows over the lock screen with the caller, Answer and Decline, and nothing of the chats; answering needs no unlock, and when the call ends the phone is back at the lock screen. The phone's own ringtone loops, with a repeating vibration, as the ringer switch and Do Not Disturb's call rules say, until the call is answered, declined, ended by the caller or 45 s pass. With the phone unlocked and the app in the background, the call shows as Android's call-style notification and rings the same way. Where Android withholds the full-screen permission, Settings says "Calls can't fill the screen" with a button to Android's page for it, and calls ring as the notification alone. Linux is unchanged. See `../spec.md`, "A call to a locked phone (Android)", user stories 42 to 51.

**Blocked by:** None (can start immediately)

**Status:** ready-for-agent

- [ ] A call window of its own, apart from the main one, shows over the lock screen and turns the screen on, on the one Flutter engine, and shows only the call screen; the main window stays off the lock screen
- [ ] Answer and Decline work from the lock screen without unlocking; after the call the window finishes and a locked phone is back at the lock screen, with the chats reachable only by unlocking
- [ ] A call-style notification on the `calls` channel while a call rings, itself silent; its Answer and Decline reach the engine whose chat peer has the call
- [ ] The phone's default ringtone loops on the ringtone stream with a repeating vibration, following the ringer mode and Do Not Disturb; it stops at the answer, the decline, the caller's `call_end` and the 45 s timeout; the app's synthesised ring is no longer played on Android
- [ ] The app checks the full-screen permission at start and when a call comes; lacking it, Settings shows the banner and its button opens Android's page
- [ ] Call tests, through the fake notifier: a call coming in with the app in the background asks to ring with the caller, and each of answer, decline, caller's end and timeout stops it; a settings test for the banner
- [ ] On a phone, written in this ticket: locked, a call from the Linux app and from the T-Deck turns the screen on and rings with the ringtone; answered without unlocking, the chats are not reachable, and the lock screen is back after the hang-up; ringer on vibrate and on silent; Do Not Disturb; the permission revoked, and the banner
