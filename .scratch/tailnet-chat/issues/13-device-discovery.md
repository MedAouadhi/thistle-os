# 13: Device: look for chat peers on the tailnet

**What to build:** the device's Chat app finds the chat peers on the tailnet, as the Linux app does (story 1), instead of only taking a name or address typed in. The Contacts page has **Look for chat peers**; it probes each tailnet peer the device sees online on port 4810 with a hello, and lists those that answer, with their name and fingerprint and an **Add** each, which adds the peer as Add contact does with its address. Asked for by the user after the first bench run (ticket 12), where the laptop found the T-Deck but the T-Deck could not find the laptop.

**Blocked by:** none (ticket 03's contacts, ticket 04's dialling)

**Status:** ready

- [ ] The peers probed are the tailnet's online peers as the kernel already keeps them (`tailnet_manager.rs`, read by the Terminal's `tailscale peers` and Hermes's Address page), less this device. A probe is what the Linux app's is (`ChatPeer.probe`, `companion/core/lib/src/peer.dart`): connect, read the hello, close, nothing held as a request on the other side. Protocol, section 3 and "Further Notes" of the spec: the device probes only when its app asks, never in the background.
- [ ] The probing runs off the UI and off the kernel worker, on a `kernel_task::spawn` task made for it and gone after (CLAUDE.md, "Internal RAM runs out first"); a few seconds' timeout a peer, a bounded number of peers, the device held awake meanwhile. The app shows that it is looking, and the results when they come (the kernel's generation counter, as the app's other pages).
- [ ] Results leave out this device and peers that are already contacts (matched by key). Each shows its hello's name, its tailnet name and its fingerprint. Add adds it by the address it answered on, so its key is pinned at the first handshake as with Add contact.
- [ ] Decisions (which peers to probe, which results to show) are host-tested in Rust; the probing in the simulator: `simulator/tests/test_tailchat.py` gains a case where the simulator's tailnet lists two reference peers on loopback (extend the simulator's pretend client, `THISTLE_SIM_TAILNET_PEERS`, as needed) and one non-chat address, the app looks, lists the two, and adds one.
- [ ] LVGL's pool is 64 KB: the results are one `lv_table`, as the app's other lists.
- [ ] CLAUDE.md's Tailnet chat lines say how discovery works on the device.

## Comments
