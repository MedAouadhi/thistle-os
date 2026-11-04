# 01: Android: an incoming call rings like a phone call

**What to build:** a call to the Android companion app rings like a phone call, whoever calls (a companion app or the T-Deck) and on today's `ws-opus` calls. On a locked phone the screen turns on and a call window shows over the lock screen with the caller, Answer and Decline, and nothing of the chats; answering needs no unlock, and when the call ends the phone is back at the lock screen. The phone's own ringtone loops, with a repeating vibration, as the ringer switch and Do Not Disturb's call rules say, until the call is answered, declined, ended by the caller or 45 s pass. With the phone unlocked and the app in the background, the call shows as Android's call-style notification and rings the same way. Where Android withholds the full-screen permission, Settings says "Calls can't fill the screen" with a button to Android's page for it, and calls ring as the notification alone. Linux is unchanged. See `../spec.md`, "A call to a locked phone (Android)", user stories 42 to 51.

**Blocked by:** None (can start immediately)

**Status:** ready-for-agent

- [x] A call window of its own, apart from the main one, shows over the lock screen and turns the screen on, on the one Flutter engine, and shows only the call screen; the main window stays off the lock screen
- [x] Answer and Decline work from the lock screen without unlocking; after the call the window finishes and a locked phone is back at the lock screen, with the chats reachable only by unlocking
- [x] A call-style notification on the `calls` channel while a call rings, itself silent; its Answer and Decline reach the engine whose chat peer has the call
- [x] The phone's default ringtone loops on the ringtone stream with a repeating vibration, following the ringer mode and Do Not Disturb; it stops at the answer, the decline, the caller's `call_end` and the 45 s timeout; the app's synthesised ring is no longer played on Android
- [x] The app checks the full-screen permission at start and when a call comes; lacking it, Settings shows the banner and its button opens Android's page
- [x] Call tests, through the fake notifier: a call coming in with the app in the background asks to ring with the caller, and each of answer, decline, caller's end and timeout stops it; a settings test for the banner
- [ ] On a phone, written in this ticket: locked, a call from the Linux app and from the T-Deck turns the screen on and rings with the ringtone; answered without unlocking, the chats are not reachable, and the lock screen is back after the hang-up; ringer on vibrate and on silent; Do Not Disturb; the permission revoked, and the banner

## Comments

Built on `tv-01-android-ring`. The seam is the `Notifier`: it now says
whether it rings itself (`ringsItself`), and `ringing` is asked for every
call that comes in, with whether the app is in front; Android's
`SystemNotifier` hands it to `CallRinger` (`android.dart`, `CallRing.kt`),
and Talk plays its two-tone ring only where the notifier does not ring.
The call window is `CallActivity.kt`; while it is attached, Dart draws
`CallWindowed` (`call_page.dart`), the call screen alone over the screens
kept offstage. Release APK builds; `flutter test` and `flutter analyze
--fatal-infos` pass.

One change from the spec: the notification's channel is `incoming_calls`,
not `calls`. flutter_local_notifications made `calls` with a sound, and
Android keeps a channel's sound (a channel deleted and made again gets its
old settings back), so `calls` is deleted and the silent one takes over.

No phone was attached, so the run below is still to do. Install the
release APK on an Android 14 or later phone with the app listening in the
background, then:

- [ ] Locked, screen off: a call from the Linux app turns the screen on and
      shows the call window over the lock screen with the caller, Answer
      and Decline, and nothing of the chats, not even for a frame as it
      opens; the ringtone loops and the phone vibrates.
- [ ] The same from the T-Deck.
- [ ] Answer from the window, still locked: the call connects with audio;
      Back or the recent apps reach no chat; after the hang-up the window
      closes and the lock screen is back.
- [ ] Decline from the window, and from the notification's Decline on the
      lock screen: the ring stops at once, the caller sees Declined.
- [ ] The caller hangs up while it rings, and a call left 45 s: the ring
      stops, the window closes, a missed call is notified.
- [ ] Unlocked, the app in the background: a heads-up call notification,
      the ringtone; its Answer opens the call window and connects.
- [ ] Unlocked, the app in front: the call screen in the app, the ringtone,
      no notification.
- [ ] Ringer on vibrate: vibration, no sound. On silent: neither, the
      window still shows.
- [ ] Do Not Disturb with calls from anyone allowed: rings. With calls from
      contacts or no calls: silent.
- [ ] Settings, Apps, Thistle Chat, full-screen notifications off: Settings
      shows "Calls can't fill the screen"; Allow opens Android's page;
      switched back on there and back in the app, the banner goes. Off, a
      locked phone gets the call as a notification that still rings.
- [ ] After a call taken over the lock screen, unlock and open the app:
      the chats show and work (the main window remade itself).
- [ ] Linux unchanged: a call in the background notifies with Answer and
      Decline and plays the two-tone ring.
