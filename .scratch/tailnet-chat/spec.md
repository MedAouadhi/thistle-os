# Tailnet chat

Status: in-review; tickets 01-11 on branch `tailnet-chat`, the hardware run (12) and the manual checks in 01, 06, 07, 10 and 11 remain

ADR-0015 (peer to peer, the tailnet trusted once, the device's MeshCore key)
has the reasons; ADR-0009 (the tailnet), ADR-0014 (background checks) and
ADR-0004 (SMS by RI) bound it. `CONTEXT.md` defines **Tailnet chat**,
**Chat peer**, **Identity key**, **Contact** and **Contact request**.

## Problem Statement

The T-Deck Max is on the user's tailnet, but nothing on it talks to the
user's other machines there except Hermes, which talks to one agent. To
message or call someone from the device the user has SMS, which costs and
needs the modem, and MeshCore, which needs LoRa range and carries no voice.
Their laptop and phone are on the same tailnet, encrypted and admitted by
WireGuard, yet there is no app on any of them that can reach the device, nor
each other, for a chat or a call.

What exists does not do it: Tailscale gives reachability and nothing above
it; the device's MicroLink client knows each tailnet peer's address and
hostname but not its user, so it cannot tell who is at the other end; and
the device has no speech codec.

## Solution

**Tailnet chat**: a protocol, an app on the device, and a companion app for
Linux and Android, so that any two chat peers on the tailnet can message
and call each other directly.

- **Peer to peer.** Each chat peer listens on a fixed port of its tailnet
  address and keeps one WebSocket per peer it is talking to. There is no
  server.
- **Contacts.** A chat peer's first message is a contact request, shown with
  its tailnet name and its key's fingerprint. Accepting it pins the key.
  After that, a peer under that contact's name or address with another key
  is refused and flagged until the user accepts the new key in place of the
  old.
- **Messages.** 1:1 text, with delivered and read receipts, "typing…",
  replies and reactions. A message for a peer that is away waits in its
  sender's outbox until it is delivered.
- **Voice.** Voice messages of up to 2 minutes, and live 1:1 calls, both in
  Opus.
- **On the device.** A new built-in app shows conversations, contacts and
  requests. The kernel receives whether or not the app is open: messages
  buzz, pop up and count like SMS, and a call rings on the system call
  screen. Every 15 minutes, unless the user changes it, a background check
  wakes a sleeping device to collect what waits for it.
- **The companion app** runs on Linux and Android from one Flutter
  codebase. It talks to the device and to other companion apps. On Android
  a foreground service keeps it reachable.
- **Room for later.** Each peer says what it can do in its hello, and a call
  offer lists the media transports it can use. Video and file transfer
  between companion apps can then come without a new protocol.

## User Stories

### Contacts and trust

1. As the user, I want my device and my laptop to find each other on the
   tailnet without typing addresses, so that starting a chat is quick.
2. As the user, I want to add a chat peer by its tailnet name or address
   when it is not discovered, such as my phone, so that I can reach it
   anyway.
3. As the user, I want to see a contact request with the sender's tailnet
   name and key fingerprint before I accept it, so that I know who is asking.
4. As the user, I want to accept or decline a contact request, so that only
   chat peers I chose can message me.
5. As the user, I want the messages that came with a contact request to
   appear once I accept it, so that nothing the sender wrote is lost.
6. As the user, I want a declined sender to be told it was not accepted,
   so that it stops retrying.
7. As the user, I want a contact's identity to stay the same when its
   tailnet address or hostname changes, so that a renamed laptop is still
   the same contact.
8. As the user, I want a chat peer that uses a contact's name or address
   with a different key to be refused and flagged, so that an impostor on
   my tailnet cannot pass for my contact.
9. As the user, I want to accept a contact's new key deliberately, after
   reinstalling an app for example, so that the conversation carries on.
10. As the user, I want to compare key fingerprints between two screens,
    so that I can check a contact in person if I care to.
11. As the user, I want to rename a contact, so that it reads as a person
    rather than a hostname.
12. As the user, I want to remove a contact, so that it has to ask again.
13. As the user, I want only connections from my tailnet admitted, so that
    nothing on the WiFi or the mobile network can reach the chat port.
14. As the user, I want the device to be the same identity on LoRa and the
    tailnet, so that it is one key to recognise.

### Messages

15. As the user, I want to send a text message to a contact, so that we can
    talk.
16. As the user, I want to see when a message is delivered and when it is
    read, so that I know where it stands.
17. As the user, I want to turn off sending read receipts, so that others do
    not see when I read.
18. As the user, I want to see when a contact is typing, so that I wait for
    the answer.
19. As the user, I want to reply to a specific message and see what a reply
    quotes, so that a conversation stays clear.
20. As the user, I want to react to a message with an emoji, and change or
    remove my reaction, so that I can acknowledge it without writing.
21. As the user, I want a message sent while the contact is away to arrive
    once it is back, without my doing anything, so that I need not resend.
22. As the user, I want to see which of my messages are still waiting, so
    that I know they have not arrived.
23. As the user, I want a message never shown twice even if it was sent
    twice, so that retries are invisible.
24. As the user, I want messages shown in the order they were written, so
    that a late delivery does not jumble the conversation.
25. As the user, I want to type in Arabic and see it drawn joined and right
    to left on the device, so that it works like everywhere else.
26. As the user, I want to dictate a message on the device with the mic key,
    so that I need not type on the small keyboard.

### Voice messages

27. As the user, I want to record a voice message and send it, so that I can
    say what is long to type.
28. As the user, I want a voice message to go through the outbox like text,
    so that it reaches a contact who was away.
29. As the user, I want to play a received voice message when I choose, with
    its length shown, so that it does not play aloud by surprise.
30. As the user, I want a recording to stop at 2 minutes, so that a message
    stays a sensible size over LTE.
31. As the user, I want a player, such as Podcasts, to pause while I record
    or play a voice message, and resume after, so that the sounds do not mix.

### Calls

32. As the user, I want to call a contact who is online and talk both ways at
    once, so that it is a real conversation.
33. As the user, I want an incoming call to ring over whatever app is in
    front, and to answer or decline it, so that I do not miss it.
34. As the user, I want to hang up, mute, and change the volume during a
    call, so that I control it.
35. As the user, I want to be told a contact is unreachable when it is
    asleep or away, so that I can leave a voice message instead.
36. As the user, I want a missed tailnet call shown like a missed phone
    call, so that I can call back.
37. As the user, I want a second call to get busy while I am in one, so that
    an ongoing call is not interrupted.
38. As the user, I want a cellular call to take priority and end a tailnet
    call, so that a phone call is never missed.
39. As the user, I want an SMS that arrives during a tailnet call over LTE to
    be fetched after the call, so that the call does not drop for it.
40. As the user, I want the far end not to hear itself back from my
    device's speaker more than is bearable, so that the call is usable on
    speaker.
41. As the user, I want calls to work over mobile data as well as WiFi,
    accepting some delay, so that I can call from anywhere.

### The device

42. As the user, I want messages and calls received while the app is
    closed, so that the app is not something I must keep open.
43. As the user, I want a received message to buzz, pop up and count in the
    status bar's envelope and Home's new-messages row, like an SMS, unless
    its conversation is on screen, so that I notice it.
44. As the user, I want the device to collect waiting messages every 15
    minutes while it sleeps, and to set that interval or turn it off, so
    that I choose between battery and delay.
45. As the user, I want the device to fetch what waits for it as soon as it
    wakes or its network comes back, so that I do not wait for the next
    check.
46. As the user, I want my history kept on the card, with the same "save
    history" switch as SMS and mesh, so that it survives restarts and I can
    read it with File Transfer.
47. As the user, I want the device kept awake while a call, a voice message
    transfer or a delivery is in progress, so that sleep does not cut it.
48. As the user, I want the device to say why chat is unavailable, such as
    the tailnet being off or not signed in, so that I know what to fix.

### The companion app

49. As the user, I want one app on my Linux laptop and my Android phone, so
    that I have the same chat everywhere.
50. As the user, I want the companion app to make its identity key on first
    start and keep it in the platform's secure storage, so that it is
    stable and not readable by other apps.
51. As the user, I want the Linux app to discover chat peers from my
    tailnet's peer list, so that the device appears by itself.
52. As the user, I want the Android app reachable in the background with a
    persistent notification, and to switch that off, so that I choose
    reachability over battery.
53. As the user, I want two companion apps, on my laptop and someone else's,
    to chat and call each other with no T-Deck involved, so that it is a
    messenger in its own right.
54. As the user, I want the companion app to tell me when Tailscale is not
    running, so that I know why nothing connects.
55. As the user, I want notifications for messages and calls in the
    companion app, so that I notice them when it is in the background.

### Development

56. As a developer, I want a written protocol with test vectors, so that a
    third implementation can be checked against it.
57. As a developer, I want the simulator to be a chat peer the Linux app can
    reach, so that I can work without hardware.
58. As a developer, I want one reference chat peer to run the same
    conformance scenarios against the simulator and the companion app's
    core, so that both stay on one protocol.
59. As a developer, I want an unknown message type or capability answered
    with an error rather than a dropped connection, so that newer peers
    work with older ones.
60. As a developer, I want the log to say what Tailnet chat does: listening,
    a peer connected and who it proved to be, a refusal and why, deliveries
    and calls, so that a problem on hardware can be read from a capture.

## Implementation Decisions

### The protocol (version 1)

- **Transport.** Plain `ws://` on TCP port 4810 of the chat peer's tailnet
  address, path `/tailnet-chat/1`. WireGuard encrypts; there is no TLS. A
  listener admits only connections from the tailnet's ranges, 100.64.0.0/10
  and fd7a:115c:a1e0::/48, and arriving on the tailnet interface; anything
  else is closed before the handshake. The simulator alone also admits
  loopback.
- **One connection per pair.** Either side dials when it has something to
  send or a reason to look. If both dial at once, the connection dialled by
  the lower identity key is kept.
- **Frames.** Control and messages are JSON text frames, each an object
  with a `type`. Audio and file data are binary frames with a small fixed
  header: a kind byte, a 32-bit stream id, a 32-bit sequence number, and
  the payload.
- **Handshake.** Each side sends `hello`: protocol version, its identity
  key (Ed25519 public key), the name it shows, its capabilities, and a
  32-byte random nonce. Each then sends `auth`: an Ed25519 signature over a
  fixed label, both nonces and both keys in dialler-then-listener order. A
  missing, late or wrong signature closes the connection. Only then does
  either side act on the other's key: a contact, a pinned contact's
  mismatch (refused with `error key_changed` and flagged), or a stranger,
  whose messages are held as a contact request.
- **Capabilities.** v1 announces `text`, `receipts`, `typing`, `reply`,
  `react`, `voice-message` and `call:ws-opus`. A peer uses only what both
  announce. An unknown `type` is answered with `error unsupported` naming
  it, and the connection stays open.
- **Messages.** `msg` carries a 128-bit id chosen by the sender, the time
  written, UTF-8 text of at most 4096 bytes, and optionally the id it
  replies to. The receiver stores it, then answers `ack` with the id: that
  is the delivered receipt and what clears the sender's outbox. A receiver
  that has seen an id acks it again and shows nothing new. `read` carries
  the ids read; it is sent only if the reader allows read receipts.
  `typing` is on or off, and lapses after 6 s without a repeat. `react`
  carries the message id and an emoji, empty to remove; reactions are
  delivered and acked like messages.
- **Contact requests.** A stranger's messages are acked (so its outbox
  clears) but held. Accepting sends `contact accepted`, and the held
  messages appear. Declining sends `contact declined` and drops them, and
  the sender stops sending to that key.
- **Voice messages.** A `voice` message carries the id, the length in ms,
  the size in bytes, the codec (`ogg-opus`) and a stream id; the Ogg/Opus
  file follows as binary chunks of at most 4 KB on that stream. It is acked
  only once whole. A broken transfer is restarted from the beginning on the
  next connection. Recorded as Opus at 16 kHz mono, at most 2 minutes
  (about 240 KB).
- **Calls.** `call_offer` carries a call id and the media transports the
  caller can use, best first; v1's only one is `ws-opus` (16 kHz mono, 20 ms
  frames). `call_answer` picks one, or declines with a reason (`declined`,
  `busy`, `unsupported`). `call_end` ends it with a reason. Audio goes as
  binary frames on the call's stream, one Opus frame each, sequence numbers
  counting frames. A receiver plays from a jitter buffer of 60 to 200 ms and
  conceals a lost or late frame with Opus' own loss concealment. 10 s
  without a frame ends the call. A call to an unreachable peer fails at
  once as `unreachable`.
- **Liveness.** WebSocket pings every 20 s while a connection is open. A
  connection with nothing to send and no call is closed after 2 minutes
  idle.
- **Later, without a new version.** Video: a `webrtc` transport in
  `call_offer`, with the SDP and ICE exchanged as JSON on the same socket,
  between peers that announce it. Files: a `file` message with chunks on a
  stream, like voice but resumable.

### Shared decisions

- **Outbox.** Every message, reaction and voice message waits in its
  sender's outbox until acked. Retries go on reconnecting, when the peer is
  seen again in a peer list, and on a backoff of 30 s doubling to 30
  minutes; nothing expires. Typing and read receipts are not queued.
- **Policy apart from I/O.** Each implementation keeps its decisions in
  one module with no I/O, tested on the host: admission, trust (pin,
  mismatch, accept a new key), the outbox's next retry, which contacts to
  dial after a rejoin, and the call rules (one at a time; a cellular call
  wins; SMS waits).

### The device

- **A kernel module, Tailnet chat**, owns the listener, the connections,
  the outbox, the contacts and the history, and runs while the tailnet is
  connected. Its sockets are non-blocking and polled by a kernel worker
  job, so it has no thread of its own; at most 4 chat connections are open
  at once. It has a C API, as other kernel services do, for the app and the
  call screen.
- **Its identity key is the MeshCore identity's**, signed with through the
  kernel's crypto. Contacts, their pinned keys, names and settings are kept
  in a small file of the module's own; nothing secret is added.
- **Settings** in `system.json` under `thistle_os.tailnet_chat`:
  `check_minutes` (15; 0 is off), `read_receipts` (on), `port` (4810).
  History follows the existing `thistle_os.messenger.save_history`.
- **History** goes through the message log as a third kind, `tailnet`: one
  JSONL file per contact, its key's fingerprint as the name, with each
  line's reply-to, reactions and delivery state; voice messages as `.opus`
  files beside it. Contact requests are kept apart until accepted.
- **Alerts** get a third kind, so a tailnet message buzzes, pops up and
  counts in the status bar's envelope and Home's new-messages row, under
  the same switch and the same rule for the conversation on screen.
- **The app** is a new built-in LVGL app (not the Messenger), with
  Conversations, Contacts and Requests. A conversation shows its latest 20
  messages, with shared styles. Lists are one `lv_table` each, as the
  App Store's and To-do's are. It only reads and asks the kernel module.
- **Calls** ring on the system call screen, extended to show a tailnet
  contact. A call runs on a task of its own, made for the call and gone
  after, with an internal stack. libopus is built for the S3 with its
  state and pseudostack in PSRAM. The call takes the audio as dictation
  does (`rs_audio_capture_begin`), sets the codec to 16 kHz mono, and holds
  the device awake. The microphone is turned down while the far end's
  frames are loud (echo suppression, not cancellation). While a tailnet
  call runs over LTE, an RI-noticed SMS is fetched after it ends. A
  cellular call that rings ends a tailnet call with `call_end` reason
  `cellular`. A missed tailnet call goes into the call log.
- **Sleep.** The module registers a background check, "tailnet chat", at
  `check_minutes`. It and every rejoin after a sleep or a network change
  dial each contact that has something in the outbox, and each contact
  seen online in the tailnet peer list, so their outboxes flush. The
  platform layer keeps each tailnet peer's online state for that, which it
  drops today. A call, a voice transfer and an outbox flush hold the device
  awake.
- **RAM.** Connection buffers and the history the app shows are in PSRAM;
  only stacks are internal. A call's task and libopus' pseudostack are
  measured on hardware before their sizes are fixed. The boot log's free
  internal RAM with the tailnet and chat on is the budget's check.

### The companion app

- **One Flutter codebase, Linux and Android first.** The protocol, the
  policy, the outbox and storage are a pure Dart package with no Flutter
  dependency, so it runs headless under `dart run`; the Flutter app is its
  UI, audio and platform glue.
- **Identity** is made on first start and kept in the platform's secure
  storage (the Secret Service on Linux, the Keystore on Android).
- **Discovery.** On Linux, from `tailscale status --json`: each online
  tailnet peer's port 4810 is tried with a hello. On Android, which has no
  peer list, by adding a name or address, and from contacts already known.
- **Listening.** Bound to the tailnet address. On Android a foreground
  service, on by default and switchable, keeps the listener and the
  connections alive.
- **Audio.** Opus at 16 kHz mono for calls and voice messages, the
  platform's echo cancellation on.
- **Storage.** A local database for contacts, conversations and the
  outbox.

## Testing Decisions

- **A good test drives a chat peer through its outside**: the protocol on
  the wire, the simulator's keys and screen, the files on its card. It does
  not reach into a module's state.
- **A reference chat peer**, stdlib Python, speaks the protocol for the
  tests. One conformance suite runs it against two targets: the simulator,
  which runs the real kernel module and app, and the companion app's Dart
  core run headless. It covers the handshake and a bad signature,
  admission, a contact request accepted and declined, a pinned key changed
  and accepted again, a message delivered, acked and deduplicated, the
  outbox across a disconnect, read receipts on and off, typing, replies,
  reactions, an unknown type answered with an error, a voice message whole
  and broken, and a `ws-opus` call answered, declined, busy and ended by a
  cellular call. Prior art: `fake_hermes_bridge.py` and `test_hermes.py`;
  `test_messenger_history.py` for the card.
- **On the simulator's side**, the same runs check the history files, the
  alert (the status bar's count), the call screen by screenshot, and that a
  call plays what the reference peer sends. The simulator's microphone is
  `THISTLE_SIM_MIC` already; its speaker gets a file it writes what it
  plays to.
- **Policy modules** are host-tested like `power_policy.rs` and
  `tailnet_policy.rs`: trust, the outbox's retries, the rejoin's dial list,
  the call rules.
- **Test vectors** beside the protocol document: a handshake with its keys,
  nonces and signatures; one of each message; binary frame headers; a short
  Ogg/Opus file and its frames. The Rust tests and the Dart tests both read
  them.
- **The device's Opus** is tested on the host by encoding and decoding a
  known clip and comparing the result's level and length.
- **Hardware** has a manual run, written down in the tickets: a call over
  WiFi and over LTE through DERP, a message to a sleeping device collected
  by the background check, the free internal RAM with a call up, and the
  Android app in the background.

## Out of Scope

- Video calls, and file and media transfer other than voice messages: the
  protocol leaves room, nothing is built.
- Group chats, and calls of more than two.
- Editing a message and deleting it for everyone.
- iOS. Nothing in the protocol rules it out, but without a push server an
  iOS peer is reachable only while the app is in front.
- End-to-end encryption inside WireGuard (ADR-0015).
- Echo cancellation on the device; v1 suppresses.
- A server or relay to hold messages while both sides are away.
- Reaching chat peers outside the tailnet.
- Moving SMS and MeshCore out of the Messenger, which the user plans
  separately.
- thistle-tk: the app and the call screen are LVGL only, like the Phone
  app.

## Further Notes

- Every chat peer gets a hello whenever another probes it, so discovery
  costs each online tailnet peer one TCP connect; the device probes only
  when its app's Contacts page asks, not in the background.
- The device's MeshCore identity is made at its first boot by the mesh
  layer. Tailnet chat needs it whether or not MeshCore is switched on; the
  first ticket confirms it exists then, or makes it.
- MicroLink enforces no ACL on what arrives, and a tailnet policy that does
  not let a peer reach port 4810 shows as unreachable. The app should say
  so rather than retry silently.
- File Transfer refuses tailnet clients by design; Tailnet chat's listener
  is the first service on the device meant to be reached over the tailnet.
- With 16 lwIP sockets, the tailnet's own, HTTP clients and up to 4 chat
  connections share them; the first ticket counts them on hardware.
- The device's history and its MeshCore key are in the clear, on the card
  and SPIFFS, like SMS and mesh today.
