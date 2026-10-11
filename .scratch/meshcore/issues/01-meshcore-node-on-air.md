# 01: A MeshCore node on air, and the Messenger speaking it

**Status:** in-review

**What to build:** The kernel runs a MeshCore chat node from boot on the board's LoRa radio, and the Messenger chats over it: the Public channel, and direct messages to contacts heard advertising. The decision is recorded in `docs/adr/0002-lora-messaging-speaks-meshcore.md`.

- [x] The node starts at boot on a board with a radio, after the saved antenna is selected
- [x] It joins MeshCore's preset for the board's band: 869.618 MHz, BW 62.5 kHz, SF8 on the T-Deck Max
- [x] It keeps its identity and contacts across a reboot
- [x] It advertises itself, and the radio reports the advert sent
- [x] The crypto its peers must agree on checks out on the device
- [x] The Messenger offers the Public channel, the contacts heard, and "Advertise this device"
- [ ] A Heltec V3 running MeshCore companion firmware sees the node, and the two exchange channel and direct messages

## How to test with a Heltec V3

1. Flash the Heltec with MeshCore's **Companion Radio (BLE)** firmware from the web flasher at flasher.meshcore.co.uk (Chrome or Edge). This replaces whatever it runs now; Meshtastic can be flashed back later.
2. Install the MeshCore app on a phone, connect to the Heltec over Bluetooth (the PIN shows on its screen), and set the radio to the **EU/UK (Narrow)** preset: 869.618 MHz, BW 62.5, SF 8. Settings → Radio on the T-Deck shows what it uses.
3. On the T-Deck: Messenger → **+** → **Advertise this device**. The app's contact list should gain `ThistleOS-XXXX` (the name Settings → Radio shows).
4. In the app, send a flood advert. Messenger → **+** on the T-Deck should list the Heltec under Contacts.
5. Public channel: send from each side; each should show the other's message.
6. Direct message: open the Heltec's contact on the T-Deck and send. The bubble reads `sending`, then a tick once the Heltec acknowledges it, or `failed` after three attempts. Reply from the app.
7. Unread messages: go to the home screen on the T-Deck and send from the app, on the Public channel and directly. Within a second the status bar shows an envelope and how many are unread. Tapping it opens the Messenger, where each conversation shows its own count; opening a conversation takes its messages off the count.

The serial console logs one line per packet (`meshcore: rx advert, flood, 112 bytes, -61 dBm`, `meshcore: tx message, direct, ...`), so a link can be checked from the console alone.

## Comments

What stood between "compiled in" and a node another device could talk to, found reading the code and then on the bench:

- **Nothing ran it.** No caller started the shim or called its loop. The engine now runs on its own task; apps post to it, and hear back through `mesh_manager`, which queues messages and delivery reports.
- **A new identity every boot**, so every peer would have seen a stranger after each restart. It is now kept in `/spiffs/config/mesh_identity`; contacts and the clock in `mesh_contacts`.
- **Callbacks registered before the mesh existed were dropped**, so nothing heard would have reached an app.
- **No Public channel.** `MAX_GROUP_CHANNELS` was undefined, which compiles channels out, and the base64 decoder for channel keys was a stub that decoded nothing.
- **Radio settings no MeshCore node uses**: 868.0 MHz, BW 125, SF7, preamble 8. The driver now sends 16 preamble symbols, as MeshCore does, and the engine tunes the radio to the preset.
- **Hardware crypto was never initialised.** The mbedtls driver's PSA calls all fail until `psa_crypto_init()` has run, and nothing ran it; MeshCore's AES wrapper ignores the error and used a buffer nothing wrote. The engine's crypto self-check caught it on the first boot (`AES-128 self-check failed`). The driver now initialises PSA, and the kernel falls back to software when the accelerator fails. This also affected the Messenger's own passphrase encryption.
- **Stack**: the self-check alone used 10.7 KB of a 12 KB task stack; it now has 20 KB, with 9.7 KB to spare after the check.

### Follow-ups, not done here

- The system clock is never set: `wifi_manager_ntp_sync()` is a stub. Timestamps this node sends start from MeshCore's baseline of May 2024 and only move forward, which peers accept, but a phone shows them as old.
- New messages arrive while the Messenger is closed and wait for it, 32 at most: the oldest go when more arrive. The status bar counts them, and those the Messenger has taken but not shown; the simulator's `--mesh-message` and `--mesh-direct` hand it messages to count. Conversations now outlive the Messenger closing, which the app manager does whenever another app comes to the front, but not a reboot.
- Adverts carry no position yet, though the board has GPS.
- Repeaters and room servers are listed by the node but not offered in the Messenger: they want a login first.
- The 915 MHz preset follows MeshCore's FAQ and is untested on hardware.
