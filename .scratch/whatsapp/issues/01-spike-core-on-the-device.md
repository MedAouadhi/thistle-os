# 01: Spike: whatsapp-rust's core on the device, measured

**What to build:** A laptop build made with `THISTLE_WHATSAPP=1` in `.env`
links the T-Deck Max to the spare number and connects to WhatsApp, and we
know what that costs. The WhatsApp part exists with its own AGPL licence
file (ADR-0013). It takes upstream `whatsapp-rust`'s core crates, pinned by
commit, and nothing from the ESP32 port. ThistleOS supplies the transport
(a TLS WebSocket over esp-tls, through a C shim, as Hermes's is), the HTTP
client and a runtime, plus a throwaway store, enough to link and connect.
The QR code is printed to the log. History sync is off and the prekey
count is cut. No sdkconfig setting changes, and every stack is in internal
RAM. A build without the option compiles and links none of it.

**Blocked by:** None (can start immediately)

**Status:** ready-for-agent (needs the user to scan the QR and to decide on the numbers)

- [ ] `THISTLE_WHATSAPP=1` in `.env` adds the part; without it the image is byte-for-byte what it was apart from the version
- [ ] The part carries its own AGPL licence file; CI and release builds cannot include it
- [ ] The boot log says it is a WhatsApp build
- [ ] The device links to the spare number by a QR code from the log, then connects and logs `OfflineSyncCompleted`
- [ ] Measured and written in this ticket's Comments: the image size against the 4.5 MB kernel slot, internal RAM free and largest block during a connect, each task's unused stack
- [ ] If it does not fit, the numbers are brought to the user, who chooses between growing the kernel slots from `recovery` and trimming the build; the choice becomes a ticket

## Comments
