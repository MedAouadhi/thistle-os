# 03: Device: contact requests, pinned keys and impostors

**What to build:** the first slice through the device. A Tailnet chat kernel module listens while the tailnet is up, its sockets polled by a kernel worker job, and proves the device's MeshCore identity in the handshake. A new built-in app shows Conversations, Contacts and Requests. In the simulator, the reference chat peer, a stranger, sends a message; it waits under Requests with its tailnet name and fingerprint; Accept makes it a contact, the message appears and is logged to the card. Decline tells the sender. Later, a peer under that contact's name or address with another key is refused and flagged, and the user can accept the new key in its place. The user can rename a contact, remove one, and see its fingerprint and the device's own.

**Blocked by:** 02

**Status:** done in the simulator; not yet seen on hardware

- [x] The module starts and stops with the tailnet, listens on `thistle_os.tailnet_chat.port` (4810), admits only tailnet addresses (and loopback in the simulator), and logs listening, each peer connected and the key it proved, and each refusal with its reason.
- [x] Trust decisions are a host-tested policy module: stranger, contact, key mismatch, a new key accepted, a removed contact.
- [x] Contacts, their pinned keys and names are kept in the module's own file and survive a restart.
- [x] A stranger's messages are acked and held as a contact request; accepting sends `contact accepted` and moves them into the conversation; declining sends `contact declined` and drops them.
- [x] Held and accepted messages go through the message log as a third kind, `tailnet`, one file per contact named by its fingerprint.
- [x] The app's lists are one `lv_table` each; it works from the keys as well as touch; it says why chat is unavailable when the tailnet is off or not signed in.
- [x] The simulator listens on 127.0.0.1 on a port it is given; `simulator/tests/` runs the conformance suite's handshake, admission, request and trust scenarios against it, and checks the card and the Requests screen.
- [x] The app launches on every simulated device profile (`test_app_matrix.py`).

## Comments

Done on branch `tailnet-chat-03-device`.

- **Layout.** `components/kernel_rs/src/tailnet_chat.rs` (the module: listener, connections, the poll job, the contacts file, the C API, the simulator's control port), `tailnet_chat_policy.rs` (admission, trust, the contacts and their file; host-tested), `tailnet_chat_wire.rs` (transcript, fingerprint, frame checking, binary header, the WebSocket and its upgrade; tested against `docs/tailnet-chat/vectors/`). C API `components/kernel/include/thistle/tailnet_chat.h`. The app is `apps/tailchat/` (`com.thistle.tailchat`, "Chat").
- **Polling.** One kernel worker job every 100 ms (`tick`); once a second it looks whether the tailnet is connected and starts or stops the listener (`look`). Connection buffers are 8 KB, allocated once, so in PSRAM on the device.
- **Identity.** Ticket 01's `mesh_identity::identity()`; the module only signs with it and verifies through the kernel's crypto.
- **The file** is `/spiffs/tailnet_chat.json` (SPIFFS: trust state should not leave with the card): contacts with pinned key, name, the name and tailnet address last seen, a flagged key, what they answered, and the outbox (only `contact` frames so far).
- **Requests on the card.** A stranger's messages go to `messages/tailnet/requests/<fingerprint>.jsonl`; Accept appends them to `messages/tailnet/<fingerprint>.jsonl` and removes the request file, Decline removes it. Requests are read back from those files at boot, so with `save_history` off a request lives only until a restart. At most 16 requests of 20 held messages; a 17th stranger's message is not acked, so it stays in its sender's outbox.
- **Beyond the spec:** the hello announces only `text` and `receipts` until 04, 08 and 09 make the rest true; `react`, `voice`, `read` and `typing` frames are ignored meanwhile and a `call_offer` is declined `unsupported`. A second connection from the same key closes the older one with 4001 (the device never dials yet, so both are dialled by the peer). The app's request page shows only the latest held message (the C API has no per-message access for requests).
- **Simulator.** `THISTLE_SIM_CHAT_PORT` (listens on 127.0.0.1, admits loopback) and `THISTLE_SIM_CHAT_CONTROL` (the control protocol, for the conformance suite's `SimTarget`) in `simulator/tests/test_tailnet_chat.py`: the conformance scenarios handshake, bad signature, unknown and invalid frames, request accepted, request declined, key changed; the card files; a restart keeping contacts and the flag; the app's Requests screen, Accept from it, and the Tailscale-is-off message. Admission of non-tailnet sources is covered by the policy's host tests only (the simulator has no second address).
- **Not seen on hardware**: the listener on the tailnet address, a peer's tailnet hostname used for the impostor check (`tailnet_manager::peer_name`), the internal RAM with chat on.
