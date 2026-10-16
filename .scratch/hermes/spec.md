# Hermes

Status: built; on the T-Deck Max, tagged `tag:hermes-device`, it reaches Hermes on homelab. The rest of the hardware pass is in issues/06

## Problem Statement

The user talks to their Hermes agent, on `homelab`, through a dedicated
device (`~/personal/hermes_esp32`, a Waveshare AMOLED board): one
conversation, Hermes's replies drawn as Markdown, its tool calls as compact
rows, its slash commands a tap away, earlier Hermes Sessions to go back to,
and the output of Scheduled Jobs on a page of its own. The T-Deck Max is
already on the same tailnet (`.scratch/tailscale/`) and has a keyboard and
dictation (`.scratch/dictation/`). It should be a second Hermes device.

## Solution

A Hermes app (`apps/hermes/`, `com.thistle.hermes`) that speaks the
Hermes Device Bridge's protocol, as hermes_esp32's firmware does, over the
tailnet:

- **Tailscale first.** The bridge listens only on a tailnet address. While
  Tailscale is off the app says so and offers to switch it on; while it
  waits for a sign-in it offers the Tailscale app; while it connects it
  says which step it is on. The app goes on by itself once the device is
  on the tailnet.
- **The bridge's address** comes from the build (`HERMES_BRIDGE_URL` in
  `.env`) or the card's `config/hermes.json`, which the app's setup page
  writes. The page lists the tailnet's peers: picking one fills in
  `ws://<its address>:8765`.
- **One Bridge Session while the app is open**: a WebSocket to the bridge,
  `HELLO` and `HELLO_ACK`, then Device Chat. Lost, it is opened again, a
  second after the first loss and up to 30 seconds apart after that.
- **Device Chat**: the bridge's last twenty messages, Markdown drawn as
  blocks; Hermes's replies, edited in place as they stream; tool calls as
  activity rows inside the turn that made them; errors as notes that are
  not Hermes's words. Enter sends the draft; Stop stops a turn; New
  conversation starts one.
- **Typed or dictated.** The compose field is an ordinary text field, so
  holding the mic key dictates into it as everywhere else; the words are
  sent when the user presses Enter. The app does not negotiate
  `spoken_message`: the transcript is the OS's, not the bridge's.
- **The Command panel** lists the commands the bridge sent; choosing one
  writes it into the draft at the cursor and sends nothing.
- **The drawer** lists this device's Hermes Sessions, asked for when it
  opens, and switches to one; it holds New conversation, Scheduled Runs
  and the bridge's address.
- **Scheduled Runs**: the jobs that have delivered something, with unread
  counts, and each job's runs, newest first, read one at a time.

## The protocol

hermes_esp32's `protocol/README.md` and
`plugins/platforms/hermes_device/bridge/service.py` are the authority.
What this app relies on:

- `ws://` only, inside WireGuard; the bridge reads no credential. It asks
  the homelab's `tailscaled` who the connecting node is and admits it only
  if it carries an approved tag, `tag:hermes-device` by default. A first
  connection from an approved node is also its Device Registration.
- `HELLO` names protocol version 1 and the capabilities `bridge_session`,
  `device_chat`, `command_descriptors`, `structured_progress`,
  `chat_history`, `session_list` and `scheduled_runs`.
- `HELLO_ACK`, then `CHAT_SYNC`, `CHAT_MESSAGE`s, `CHAT_COMMANDS` pages and
  `CHAT_SYNC_COMPLETE`. Every frame after is applied as it arrives:
  `CHAT_MESSAGE`, `CHAT_MESSAGE_REPLACE` (the parts of an edited message
  are removed, then sent again), `CHAT_ACTIVITY`, `CHAT_RESET`,
  `CHAT_ACCEPTED`, `SESSION_LIST`, `SCHEDULED_JOBS`, `SCHEDULED_RUN`,
  `SCHEDULED_RUNS_UNAVAILABLE`, `ERROR`.
- A message longer than 700 bytes arrives as Renderable Parts, `id`,
  `id.1`, `id.2`, each valid Markdown on its own, each drawn as its own
  row and never joined.
- `HELLO_ACK`'s `timezone` is ignored: ThistleOS has its own (Settings ->
  Date & time).
- The bridge pings every 20 seconds and closes a session whose pong does
  not come within 20; the app answers pings, and takes 50 seconds without
  a frame of any kind for a lost link.

## Setting the device up

The device joins the tailnet as the user's own device, untagged, by the
Tailscale app's sign-in. The bridge refuses an untagged node with
`AUTHENTICATION_FAILED`. In the Tailscale admin console:

1. Machines -> the device (`thistle-xxxxxx`) -> Edit ACL tags ->
   `tag:hermes-device`. The node keeps its keys; it is owned by the tag
   from then on.
2. The tailnet policy must let `tag:hermes-device` reach the bridge's
   address and port (`homelab`, `100.125.204.43:8765`), as it already does
   for the AMOLED device. A packet the policy drops is answered with
   nothing, so the app sees a connect that times out.

The app says both when it is refused or cannot connect.

## Memory

- The chat state, the receive buffer (64 KB, the bridge's own frame
  limit) and the outbox are in PSRAM, allocated when the app opens and
  freed when it closes.
- The session runs on one FreeRTOS task with an 8 KB stack in internal
  RAM, which exists while the app is open and the tailnet is up.
- The transcript's widgets use shared styles and `lv_label_set_text_static`
  over the app's own PSRAM copies, to keep LVGL's 64 KB pool for objects.
- The device is held awake while a turn is in flight (submitted, a tool
  running, or a reply still streaming), for ten minutes at most.

## Decisions

- **A WebSocket client of the app's own** (`hermes_ws.c`, RFC 6455 client
  side, `ws://` only) over BSD sockets, rather than `esp_websocket_client`:
  one code path on the device and in the simulator, whose sockets are the
  host's, and framing that is tested on the host. The handshake's SHA-1 is
  `pod_sha1.c`, the JSON reader `pod_json.c`, both from the Podcasts app.
- **Markdown by md4c**, as hermes_esp32 does, with its
  `hermes_markdown.c` ported: blocks and spans, GFM tables. A table that
  fits the width is a grid; one that does not becomes `Header: value`
  groups (hermes_esp32 ADR 0050). Inline emphasis is not drawn: there is
  no bold face in the build.
- **Characters the fonts lack** are written as the nearest ASCII where
  there is one (dashes, quotes, accented Latin letters, arrows), and
  emoji with an LVGL symbol become it; other emoji, and their joiners and
  selectors, are dropped; anything else is `?`. Arabic is drawn as in
  every other app.
- **The session lives with the app.** Leaving the app closes it; opening
  the app again syncs from the bridge's projection and Hermes's history.
  hermes_esp32 is a single-purpose device that is always connected.
- **No offline queue.** A draft is sent only while the session is up, as
  the bridge requires (hermes_esp32 ADR 0027); otherwise it stays in the
  field and a toast says why.

## Out of scope

- Spoken Messages through the bridge: dictation is the OS's.
- Hermes's approval prompts and structured prompts, which the bridge does
  not send yet.
- Notifications of a reply while the app is closed.
- The Home Timezone.
- `wss://`, and a bridge outside the tailnet.

## Tickets

`issues/01` to `06`.
