# 03: Device: contact requests, pinned keys and impostors

**What to build:** the first slice through the device. A Tailnet chat kernel module listens while the tailnet is up, its sockets polled by a kernel worker job, and proves the device's MeshCore identity in the handshake. A new built-in app shows Conversations, Contacts and Requests. In the simulator, the reference chat peer, a stranger, sends a message; it waits under Requests with its tailnet name and fingerprint; Accept makes it a contact, the message appears and is logged to the card. Decline tells the sender. Later, a peer under that contact's name or address with another key is refused and flagged, and the user can accept the new key in its place. The user can rename a contact, remove one, and see its fingerprint and the device's own.

**Blocked by:** 02

**Status:** ready-for-agent

- [ ] The module starts and stops with the tailnet, listens on `thistle_os.tailnet_chat.port` (4810), admits only tailnet addresses (and loopback in the simulator), and logs listening, each peer connected and the key it proved, and each refusal with its reason.
- [ ] Trust decisions are a host-tested policy module: stranger, contact, key mismatch, a new key accepted, a removed contact.
- [ ] Contacts, their pinned keys and names are kept in the module's own file and survive a restart.
- [ ] A stranger's messages are acked and held as a contact request; accepting sends `contact accepted` and moves them into the conversation; declining sends `contact declined` and drops them.
- [ ] Held and accepted messages go through the message log as a third kind, `tailnet`, one file per contact named by its fingerprint.
- [ ] The app's lists are one `lv_table` each; it works from the keys as well as touch; it says why chat is unavailable when the tailnet is off or not signed in.
- [ ] The simulator listens on 127.0.0.1 on a port it is given; `simulator/tests/` runs the conformance suite's handshake, admission, request and trust scenarios against it, and checks the card and the Requests screen.
- [ ] The app launches on every simulated device profile (`test_app_matrix.py`).
