# WhatsApp

Status: ready-for-agent

ADR-0013 (an AGPL part kept out of releases) and ADR-0014 (background
checks) have the reasons. `CONTEXT.md` defines **Linked device** and
**Background check**.

## Problem Statement

The user's conversations are on WhatsApp, not SMS or MeshCore. On the
T-Deck Max they can read and send SMS and mesh messages, but WhatsApp means
taking out the phone. There is no official WhatsApp client for a
microcontroller, and the open-source one that runs on an ESP32,
`oxidezap/whatsapp-rust` with its ESP32 port, does not fit ThistleOS as it
is. Its Signal code is AGPL, while ThistleOS takes no GPL dependency. Its
port puts thread stacks in PSRAM and changes global sdkconfig settings. Its
session store needs about 1 MB, and the device's NVS has 24 KB. A
connection held open all the time would also cost about 60 KB of the
roughly 150 KB of internal RAM free at boot.

## Solution

A build made with `THISTLE_WHATSAPP=1` in the laptop's `.env` turns the
device into a **linked device** of the user's WhatsApp account. No release
includes it.

- **Linking.** The WhatsApp app shows WhatsApp's QR code, to be scanned
  from the phone under Linked devices. "Link with phone number" is the
  alternative: it shows a code that is typed on the phone.
- **Background checks.** Once linked, the kernel checks for messages at the
  interval set in WhatsApp's settings (5, 15, 30 or 60 minutes, or Off; 15
  by default). It wakes a light-sleeping device to do so. A check connects,
  takes the messages the server kept, logs them, and disconnects. A direct
  message, or a group message that mentions or replies to the user, buzzes,
  pops up and counts in the status bar's envelope and Home's new-messages
  row, as an SMS does.
- **The app.** It lists chats and opens a chat. While it is open it holds a
  live connection, so messages arrive at once and the background check is
  paused. In a chat the user can:
  - read and send text, in direct chats and groups;
  - reply to a message;
  - react to a message;
  - see and send "typing…".

  Opening a chat sends read receipts. Media, voice notes, stickers and
  other kinds of message show as placeholders such as `[photo]`. A message
  can be sent only while connected; offline, the app says so.
- **Unlinking and bans.** If WhatsApp unlinks the device, the store is
  erased, checks stop, and the app offers to link again. A temporary ban
  stops checks until it ends and says so. Nothing retries in a loop.
- **Updates.** Before installing a release, Settings → System update warns
  that the release has no WhatsApp. After installing, the user flashes
  their own build again by USB, and the linked device is picked up again
  from the card.

## User Stories

1. As the user, I want to link the device to my WhatsApp account by
   scanning a QR code on its screen, so that it can send and receive as me.
2. As the user, I want to link with a pairing code typed on my phone
   instead, so that I can link when scanning the e-paper screen is awkward.
3. As the user, I want the QR code to refresh as WhatsApp issues new ones,
   so that a slow scan still succeeds.
4. As the user, I want the app to say when linking succeeded and show my
   account's name, so that I know which account the device acts as.
5. As the user, I want the device to stay linked across restarts, sleeps,
   deep sleeps and power-offs, so that I link it only once.
6. As the user, I want new WhatsApp messages fetched in the background
   while the app is closed, so that I hear about them without opening it.
7. As the user, I want to choose how often the background check runs, from
   5, 15, 30 or 60 minutes or Off, so that I trade promptness against
   battery.
8. As the user, I want the background check to wake a sleeping device and
   let it sleep again, so that messages arrive while it lies idle.
9. As the user, I want a direct message to buzz and pop up, so that I
   notice it.
10. As the user, I want a group message to buzz and pop up only when it
    mentions me or replies to me, so that busy groups do not wear the
    battery or my patience.
11. As the user, I want unread WhatsApp messages counted in the status
    bar's envelope and Home's new-messages row, so that I see at a glance
    that something came.
12. As the user, I want to see my chats, most recent first, with each
    one's name, last message and unread count, so that I can pick what to
    read.
13. As the user, I want chat names from WhatsApp's push names, falling back
    to my phonebook by number, so that chats show names I recognise.
14. As the user, I want to open a chat and read its recent messages, mine
    and theirs, with senders shown in groups, so that I can follow it.
15. As the user, I want to type or dictate a reply and send it, so that I
    can answer from the device.
16. As the user, I want to reply to a particular message, so that the
    other side knows what I am answering.
17. As the user, I want to react to a message, so that I can answer
    without typing.
18. As the user, I want to see when the other side is typing, and have
    them see when I am, so that a conversation feels live.
19. As the user, I want opening a chat to mark its messages read on
    WhatsApp, so that my phone and the senders agree on what I have seen.
20. As the user, I want photos, voice notes, stickers and other media
    shown as placeholders like `[photo]`, so that I know something came
    even though the device cannot show it.
21. As the user, I want messages to arrive at once while the app is open,
    so that a conversation in progress does not wait for the next check.
22. As the user, I want the app to tell me when it cannot send because
    there is no connection, so that I do not think a message went when it
    did not.
23. As the user, I want WhatsApp to work over WiFi and mobile data, so that
    it works away from home.
24. As the user, I want my WhatsApp history kept on the card, and read
    back when the app opens after a restart, so that a restart does not
    empty my chats.
25. As the user, I want the Messenger's "save history" setting to govern
    WhatsApp's history too, so that one switch keeps or stops all message
    history.
26. As the user, I want the linked device's keys on the card to be
    unreadable without this device, so that a lent or lost card does not
    give my account away.
27. As the user, I want the app to tell me when WhatsApp has unlinked the
    device and offer to link again, so that I am not left wondering why
    nothing arrives.
28. As the user, I want to be told when WhatsApp has temporarily banned
    the number, and until when, so that I know what happened and when it
    ends.
29. As the user, I want the device never to retry in a loop after an
    unlink or a ban, so that it does not make matters worse with Meta.
30. As the user, I want to unlink the device from the app, so that I can
    stop it acting as me without my phone.
31. As the user, I want Settings to warn me before installing a release
    that has no WhatsApp, so that I am not surprised when it is gone.
32. As the user, I want my linked device picked up again when I flash my
    own build after a release, so that I need not link again.
33. As the developer, I want WhatsApp built only when my `.env` sets
    `THISTLE_WHATSAPP=1`, so that releases stay BSD-3 and never contain
    AGPL code.
34. As the developer, I want a WhatsApp build to say so in its boot log
    and in Settings, so that I know which kind of build a device runs.
35. As the developer, I want each background check logged with its source,
    the messages it took, how long it took and the internal RAM free, so
    that I can tune its cost.
36. As the developer, I want a check skipped and logged when the clock is
    unset, no network is up, or too little internal RAM is free, so that a
    check never fails half-way or starves the system.
37. As the developer, I want the background check service open to any
    kernel module, so that the next module that wants one does not start
    from nothing.
38. As the developer, I want the simulator to run the app, the checks and
    the alerts against a fake WhatsApp, so that CI tests the feature
    without touching Meta's servers.
39. As the developer, I want to try it against a spare number first, so
    that a ban during development costs nothing.

## Implementation Decisions

- **An optional part.** WhatsApp is a part with its own AGPL licence
  file, made of the kernel module, the app and their glue. It is compiled
  only when the build reads `THISTLE_WHATSAPP=1` from `.env`, as the other
  `.env` keys are read. CI and release builds have no `.env`. Without the
  option, nothing of it is compiled or linked, and the background check
  service, which is BSD, has no WhatsApp check registered.
- **Upstream's core, our glue.** The module depends on upstream
  `whatsapp-rust`'s core crates, pinned by commit, and on none of the ESP32
  port's code or its dependencies (esp-idf-svc). ThistleOS supplies four
  things itself:
  - the transport: a TLS WebSocket over ESP-IDF's esp-tls, through a C
    shim, as Hermes's and the HTTP stream's are;
  - the HTTP client;
  - the store backend;
  - a runtime that runs the library's futures on the task it is given.
  No sdkconfig setting changes, and every stack is in internal RAM.
  History sync is off, and the number of prekeys uploaded is cut, as the
  port does.
- **The flash budget is measured first.** The first ticket is a spike: a
  build with the core linked and a connect exercised, to measure the
  kernel image against its 4.5 MB kernel slot (3.73 MB today) and the
  internal RAM a check holds. What to do if it does not fit, whether to
  grow the slots from `recovery` or trim, is decided with the user on its
  numbers.
- **The WhatsApp client interface** is the module's one seam, the only
  place whatsapp-rust is called from:
  - in: link by QR or by code, connect, disconnect, unlink, send text,
    send a reply, send a reaction, send a read receipt, send typing;
  - out, as events: QR code, pairing code, linked (own name and number),
    message (chat, sender, push name, kind, text, quoted id, mentions me,
    timestamp), receipt, typing, offline queue done, unlinked, temporarily
    banned (until when), replaced, disconnected.
  The device's implementation drives whatsapp-rust on its own FreeRTOS
  task. The simulator's is a fake chosen by `THISTLE_SIM_WHATSAPP`.
- **The background check service** is a kernel module with a policy half
  and a running half, as the power and update modules are.
  - A kernel module registers a check with a name, an interval it can
    change, and a function. Packaged apps cannot register checks.
  - The policy decides whether a check is due: its interval has passed, a
    network is up, the clock is set, it is not paused, it is not stopped
    until some time, and the internal RAM it declares is free.
  - The running half runs a due check on a FreeRTOS task made for it with
    `kernel_task::spawn` and gone after, holding the device awake.
  - It tells the power manager when the next check falls, so light sleep
    wakes on a timer for it. Deep sleep runs none.
  - A check returns how it went: done, skipped and why, failed, or stopped
    until some time.
- **The WhatsApp check** connects, waits for "offline queue done" or a
  timeout, hands each message to the message handling below, and
  disconnects. While the app holds a live connection, the check is paused.
- **Message handling** is the same for a check and the live app. Each
  message is appended by `msg_log` under a new kind, `whatsapp`, one
  file per chat, unless `thistle_os.messenger.save_history` is off.
  `msg_alert` alerts a direct message, or a group message that mentions or
  replies to the user, unless the app has that chat on screen. The
  envelope counts unread WhatsApp messages with SMS and mesh.
- **Unlink, ban and replaced** are a pure policy:
  - unlinked: erase the store and stop the check until linked again;
  - temporarily banned: stop the check until the ban's end, and pop up
    once;
  - replaced: log it and wait for the next check.
  None of them retries sooner.
- **The store** keeps whatsapp-rust's store traits in files under
  `/sdcard/whatsapp/`. Each file is encrypted and authenticated with a
  random 256-bit key generated at linking and kept in the internal `nvs`,
  until the credential partition of ADR-0012 exists to take it. A file
  that does not authenticate is treated as missing, never as plain text.
  Unlinking erases the files and the key.
- **Settings** live in `system.json` under `thistle_os.whatsapp`: the
  interval in minutes, 0 for Off. Whether the device is linked is the
  store's to say, not a setting's.
- **The app** is a built-in LVGL app, registered only in a WhatsApp build.
  - Its pages are the link page (QR, with the pairing code alternative),
    the chat list, the chat, and settings (interval, unlink).
  - Lists are one `lv_table` each and text sits in PSRAM, because of
    LVGL's 64 KB pool.
  - Its live connection is held only while it is open, and the device is
    held awake meanwhile.
  - The compose field is an ordinary text field, so dictation works in it.
  - Names come from push names, with the phonebook by number as the
    fallback.
- **The update warning.** A WhatsApp build marks itself through a flag the
  update manager can read. Settings → System update says "this release has
  no WhatsApp" before Download and install or Install from card proceeds.

## Testing Decisions

- A good test drives what the user or a module sees, through the highest
  seam that reaches it, and never reaches into internals.
- **The simulator, through the WhatsApp client interface.**
  `simulator/tests/test_whatsapp.py` runs the simulator with
  `THISTLE_SIM_WHATSAPP` scripting the fake (link by QR, link by code,
  queued messages, unlinked, banned) and checks:
  - the app's pages;
  - that a background check takes messages and alerts them;
  - that groups alert only on a mention;
  - the envelope and Home's row;
  - that `msg_log` files are written and read back;
  - that a check is paused while the app is open;
  - what unlink and ban do.
  The prior art is `test_tailscale.py` (a pretend client chosen by
  environment) and `test_hermes.py` (an app against a fake peer).
- **Host-tested policy**, in the manner of `update_policy`'s and
  `power_policy`'s tests:
  - when a background check is due, skipped, paused or stopped, and when
    the next wake falls;
  - which messages alert;
  - what unlinked, banned and replaced do.
- **The store, on the host**, against whatsapp-rust's store traits:
  - what is saved reads back;
  - a wrong key or a damaged file reads as missing;
  - unlinking erases everything.
- **On the device, with the spare number, by hand**, as ticket notes:
  - linking both ways;
  - a check from light sleep;
  - internal RAM held during a check;
  - the flash size;
  - a ban-free week of normal use before the main number is linked.

## Out of Scope

- Any release containing WhatsApp, and any change to ThistleOS's licence.
- Downloading, showing or sending media, voice notes or stickers.
- History sync of chats from before linking.
- Calls, statuses, communities, newsletters, polls and editing messages.
- A live connection held while the app is closed.
- WhatsApp chats in the Messenger.
- Background checks for packaged apps.
- An outbox for messages sent offline.
- Growing the kernel slots or trimming the build, until the spike's
  numbers are in.

## Further Notes

- WhatsApp may ban numbers that use unofficial clients. Develop against a
  spare number; link the main number only after it behaves.
- Upstream's `wacore/libsignal` says MIT in its `Cargo.toml`, but its
  sources are Signal's, AGPL-3.0-only (ADR-0013). Worth reporting upstream.
- The Noise handshake needs the clock set, which is why a check waits for
  it.
- `whatsapp-rust-esp32`'s measurements are the starting point: about 53 KB
  of internal RAM at connect, and peak stacks well under its sizes (main
  about 30 KB). Re-measure on the T-Deck Max with our glue.
