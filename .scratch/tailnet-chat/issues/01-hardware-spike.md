# 01: Hardware spike: a listener on the tailnet, and Opus on the S3

**What to build:** proof on the T-Deck Max that the two things Tailnet chat rests on hold, before anything is built on them. A throwaway listener on the device's tailnet address answers a WebSocket opened from the Linux laptop, over WiFi and over LTE through DERP. libopus is built for the device and the simulator and encodes and decodes speech at 16 kHz mono in 20 ms frames, with its state and pseudostack in PSRAM. The numbers the later tickets size things by are measured and written down here. The spec is `.scratch/tailnet-chat/spec.md`; ADR-0015 has the reasons.

**Blocked by:** None (can start immediately)

**Status:** ready-for-agent

- [ ] From the laptop, a WebSocket to the device's tailnet address and a throwaway port completes its handshake and echoes a frame, over WiFi and over LTE; the time to first byte on each is written down.
- [ ] A connection to that port from the WiFi network, not the tailnet, is refused, and File Transfer still refuses tailnet clients.
- [ ] The tailnet platform layer keeps each tailnet peer's online state, which it dropped, and the kernel's peer list exposes it.
- [ ] It is known whether the MeshCore identity exists on a board that has never had MeshCore switched on; if not, it is made at boot whatever the mesh setting.
- [ ] The lwIP sockets in use with the tailnet up and 4 chat connections open are counted against the limit of 16.
- [ ] libopus is a component built for the firmware and the simulator; a host test encodes and decodes a known clip and checks its length and level.
- [ ] On hardware: CPU time per 20 ms frame to encode and to decode, the stack a call task needs, and the pseudostack's size, written in this ticket's comments.
- [ ] Internal RAM free with the tailnet on, before and after the listener and an encoder and decoder are made, in the comments.
- [ ] The throwaway listener is removed or left behind a debug switch that is off.
