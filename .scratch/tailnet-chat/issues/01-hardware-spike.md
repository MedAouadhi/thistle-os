# 01: Hardware spike: a listener on the tailnet, and Opus on the S3

**What to build:** proof on the T-Deck Max that the two things Tailnet chat rests on hold, before anything is built on them. A throwaway listener on the device's tailnet address answers a WebSocket opened from the Linux laptop, over WiFi and over LTE through DERP. libopus is built for the device and the simulator and encodes and decodes speech at 16 kHz mono in 20 ms frames, with its state and pseudostack in PSRAM. The numbers the later tickets size things by are measured and written down here. The spec is `.scratch/tailnet-chat/spec.md`; ADR-0015 has the reasons.

**Blocked by:** None (can start immediately)

**Status:** done but for: the echo over LTE through DERP, and File Transfer refusing a tailnet client on hardware (both manual, below); and MicroLink keeping the control plane's online state, a commit to the fork (`microlink-peer-online.patch` beside this folder's spec), until which every peer reads online.

- [ ] From the laptop, a WebSocket to the device's tailnet address and a throwaway port completes its handshake and echoes a frame, over WiFi and over LTE; the time to first byte on each is written down. *(WiFi done and measured; LTE manual.)*
- [ ] A connection to that port from the WiFi network, not the tailnet, is refused, and File Transfer still refuses tailnet clients. *(The first seen on hardware; File Transfer unchanged, its check read, the hardware run manual.)*
- [ ] The tailnet platform layer keeps each tailnet peer's online state, which it dropped, and the kernel's peer list exposes it. *(Kept and exposed; MicroLink at the pinned commit says online for every peer, so the fork needs the patch.)*
- [x] It is known whether the MeshCore identity exists on a board that has never had MeshCore switched on; if not, it is made at boot whatever the mesh setting.
- [x] The lwIP sockets in use with the tailnet up and 4 chat connections open are counted against the limit of 16.
- [x] libopus is a component built for the firmware and the simulator; a host test encodes and decodes a known clip and checks its length and level.
- [x] On hardware: CPU time per 20 ms frame to encode and to decode, the stack a call task needs, and the pseudostack's size, written in this ticket's comments.
- [x] Internal RAM free with the tailnet on, before and after the listener and an encoder and decoder are made, in the comments.
- [x] The throwaway listener is removed or left behind a debug switch that is off.

## Comments

Measured on the T-Deck Max (thistle-46cf20, 100.110.81.38) on 2026-11-01/02, over WiFi on the same LAN as the laptop (192.168.2.0/24), the tailnet's path direct (`tailscale status`: `direct 192.168.2.230:51820`). The board had no SIM, so nothing went over LTE.

### The spike

`components/kernel/src/tailnet_chat_spike.c`, behind `CONFIG_THISTLE_TAILNET_CHAT_SPIKE` (menuconfig, "ThistleOS debug"), off by default; the S3 only. On, it switches WiFi and the tailnet on if they are off (saved, as Settings would), holds the device awake, and while the tailnet is connected: listens on TCP 4810 on any address, answers a WebSocket on any path and echoes its frames, admitting only a peer in 100.64.0.0/10 whose connection arrived on the device's tailnet address; logs the sockets and the internal RAM as connections come and go, and the peer list once a minute; and once, on a task with a 24 KB stack, times Opus. All of it is a kernel worker job every 10 ms with non-blocking sockets, as the module will be. Its log tag is `chat_spike`. To turn it on, set `CONFIG_THISTLE_TAILNET_CHAT_SPIKE=y` in `sdkconfig` and build. The laptop's probe is `.scratch/tailnet-chat/ws_probe.py`, a stdlib WebSocket client.

### The echo over WiFi

| | TCP connect | first byte of the 101 | echo, 20-30 B text | 70 B binary |
|---|---|---|---|---|
| first connection after boot (WireGuard handshake) | 605-718 ms | 656-746 ms | 15-28 ms | 17-35 ms |
| next connections | 7-107 ms | 32-132 ms | 15-29 ms | 15-23 ms |

The device answered the handshake 5-12 ms after its accept. The echo's 15-28 ms includes up to 10 ms of the job's poll and WiFi power save. A first connection's ~0.6 s is MicroLink starting a WireGuard session for a packet that found none (CLAUDE.md, Tailscale).

**Remaining, manual:** the same over LTE, which goes through DERP: a SIM in the board, WiFi off, then `python3 .scratch/tailnet-chat/ws_probe.py 100.110.81.38 4810 --conns 4` from the laptop, with the spike on.

### Admission

From the laptop's LAN address to the device's WiFi address, 192.168.2.131 to 192.168.2.230:4810: the TCP connection was accepted by lwIP and closed at once, before any byte (`refused 192.168.2.131, to 192.168.2.230: not from the tailnet`); the client saw a reset. The module should check the same two things: the peer in 100.64.0.0/10 (or fd7a:115c:a1e0::/48) and the local address the tailnet's.

File Transfer is unchanged: its `on_open` (apps/filexfer/filexfer/xfer_server.c) closes any connection whose local address is not the WiFi address, so a tailnet client is refused. **Remaining, manual:** open File Transfer on the device and `curl http://100.110.81.38/` from the laptop; it must fail, while `http://192.168.2.230/` answers.

### Sockets

With the tailnet connected (and everything else a T-Deck Max runs at boot), 3 of the 16 lwIP sockets were open; the listener made 4; 4 chat connections made 8. So the tailnet and the listener take 4, and 4 connections leave 8 for the rest: an HTTP client takes 1 while it runs (the daily check, AssistNow, the App Store, Hermes' WebSocket, Podcasts' stream), File Transfer's server several (httpd's own sockets plus one per client). 4 chat connections fit; a 5th and 6th were not tried against the others all at once.

### Internal RAM

| | free |
|---|---|
| the tailnet connected, nothing of the spike | 106-112 KB (varies by boot) |
| after the listener | unchanged (106 / 112 KB) |
| 4 chat connections open (buffers in PSRAM) | 111 KB (from 112) |
| after making a task with a 24 KB stack | 81-87 KB |
| after making an encoder and a decoder (PSRAM) | unchanged |

The codecs cost no internal RAM: their state (encoder 24,548 B, decoder 17,800 B) and the pseudostack (20 KB) are in PSRAM. The call task's stack is the cost.

### Opus on the S3 (240 MHz, fixed point, 16 kHz mono, 20 ms frames, 24 kbit/s VBR, FEC on)

CPU time per frame over 100 frames of the speech-like clip the host test uses:

| complexity | encode, mean (max) | decode, mean (max) | lost frame concealed | bytes for 2 s |
|---|---|---|---|---|
| 9 (libopus' default) | 22.3 ms (29.1) | 1.4 ms (2.2) | 1.6 ms | 3088 |
| 5 | 14.3 ms (18.3) | 1.4 ms (2.0) | 1.4 ms | 3047 |
| 3 | 10.9 ms (15.4) | 1.4 ms (3.6) | 1.4 ms | 2983 |
| 1 | 6.2 ms (8.8) | 1.4 ms (3.6) | 1.2 ms | 2980 |
| 0 | 5.8 ms (7.9) | 1.3 ms (1.9) | 1.4 ms | 2496 |

Complexity 9 cannot keep up (22 ms of CPU per 20 ms of audio); `thistle_opus_encoder_new` sets complexity 1 (`THISTLE_OPUS_COMPLEXITY`): a call's encode and decode take about 7.6 ms of every 20, 38 % of a core. Moving the pseudostack to internal RAM saved 3-5 % (complexity 1: 5.95 ms against 6.24 ms), not worth its 20 KB.

- **Pseudostack:** 14,076 B used at most (host, 64-bit: 14,816 B), over complexities 9 to 0, encoder and decoder together. `GLOBAL_STACK_SIZE` is now 20,480 B. libopus does not bound-check it.
- **The call task's stack:** the Opus task used 14,444 B of its 24,576 B (1,276 B before the first codec call), libopus' own locals, beyond the pseudostack. A call task that also sends and receives frames should get **20 KB**, measured again once it exists.
- The pseudostack is one buffer every encoder and decoder shares (`NONTHREADSAFE_PSEUDOSTACK`): one task may use Opus at a time.

### Peer online

The kernel's peer list carries `online` now (`tailnet_peer_t.online`, the Terminal's `tailscale peers` says `offline`), from the platform layer, which dropped MicroLink's. But MicroLink at the pinned commit (709ba05) sets `online` for every peer in its map: its `online` is `active`, a slot in use. On the device all 9 peers read online while `tailscale status` had 7 of them offline. MicroLink does not read the control plane's `Online` at all, nor `OnlineChange`, nor `PeersChangedPatch` as Tailscale sends it (an array by NodeID; MicroLink only handles an object keyed by node key, so it ignores Tailscale's patches altogether).

`.scratch/tailnet-chat/microlink-peer-online.patch` (paths under the fork's `components/microlink/`) keeps each peer's node id and `Online` from the netmap, and takes `OnlineChange` and the array form of `PeersChangedPatch`. Built into this firmware by hand, the device read 2 of 9 online, homelab and the laptop, as `tailscale status` had them. **Remaining:** commit it on the fork's `thistle` branch, push, and move the pin in `components/kernel/idf_component.yml` (deleting `dependencies.lock`). Until then `online` is true for every peer, which only makes the rejoin dial more than it need.

### The MeshCore identity

It did not exist on such a board: only the MeshCore engine made it, in `meshcore_start`, which runs only with the mesh on and a radio. Now `mesh_identity.rs` loads or makes `/spiffs/config/mesh_identity` at boot, before the mesh, whatever its setting, in the engine's layout, which reads it. This board had the mesh off but one already, from when it was on: the boot log says `identity key 11f78506f79de864...`, and a new one would say `made a new identity key`.
