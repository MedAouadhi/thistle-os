# Join a tailnet with MicroLink, paired by a sign-in URL

ThistleOS joins the user's tailnet (Tailscale) so that the device reaches their own services, a Hermes agent first among them, by name and over WireGuard, from any network. Three things had to be decided: which client, how a device is paired without a secret in the firmware, and what the tailnet is to the rest of the kernel.

**The client is MicroLink**, a C ESP-IDF component, taken from the fork that hermes_esp32 pins (`MedAouadhi/microlink`) with five commits on a branch of its own, `thistle`, and pinned by commit in `components/kernel/idf_component.yml`. It is built only for the S3, whose PSRAM it allocates from. It already runs on this SoC in another project, and it gives lwIP a WireGuard netif for 100.64.0.0/10, so ordinary sockets, `esp_http_client` included, reach a peer with no API of its own. The six commits: it builds against mbedTLS 3 (ESP-IDF 5.5) as well as 4; a stop waits for its tasks and frees what they held, so the tailnet can be switched off and on; a registration without a key signs in at a URL and a refused one says why; the home DERP region is the nearest by measurement rather than Dallas; the WireGuard netif is torn down on the TCPIP thread; and a packet that finds no WireGuard session starts one, as `microlink_tcp_connect()` did for its own sockets, so a plain socket reaches a peer that has not reached the device first. Each is written up in its commit message, and offerable upstream as it stands.

**Pairing compiles nothing in.** A device that has never joined starts without a key; the control plane answers with a URL, which the Tailscale app shows as a QR code, and the client waits on it (`Followup`) until someone signs in there. That is Tailscale's own interactive login, and nothing secret passes through the device. After the first join the device keeps its own identity, MicroLink's keys in NVS, and rejoins with no sign-in.

**The tailnet is an overlay of its own**, not a `hal_net_driver_t` transport. net_manager's VPN transport type means a VPN that carries all traffic, and `net_get_active()` would then name the tailnet for traffic that does not go through it, and the status bar with it. The tailnet manager instead follows the underlay net_manager reports, and restarts the client whenever that changes, so its sockets are never left on an interface that has gone.

## Considered Options

**tailscale-mpe-rust** (punnerud), a Rust client, has no licence, which rules it out alone. It also gives the OS no netif, so every protocol would be hand-built in its data path; its threads want about 136 KB of stack, about what a T-Deck Max has free; and it does not check that a handshake's key belongs to the tailnet.

**Upstream MicroLink** has none of the fork's repairs: decrypted packets handed to lwIP off its thread, which aborted hermes_esp32's board, and a map response misparsed when its length ends in `{`. Its later Wi-Fi and cellular switching would decide the bearer, which the network coordinator does.

**An auth key, from a file, typed, or compiled in.** A key in `tailscale.key` on the SD card, used and deleted once the control plane had accepted or refused it, was built and tested in the simulator, and taken out on the owner's call, to keep one way in for now; the URL needs no secret on the device at all. MicroLink still takes a key, so it can come back. Typed, a 60-character key is error-prone and lands in a text field's history; compiled in, it is in every copy of the image, as hermes_esp32's ADR 0055 says of the same Kconfig fields, which are pinned empty here too.

**MicroLink's own HTTP configuration server** would serve the auth key to the tailnet and is the only way to point it at a control plane other than Tailscale's. It stays off (`CONFIG_ML_ENABLE_CONFIG_HTTPD=n`).

## Consequences

A device signed in by URL belongs to the user who signed in, and its key expires as their devices' do, 180 days by default, unless expiry is disabled for it in the admin console. When it expires the client makes a new node key, sends the old one as `OldNodeKey`, and the device asks for a sign-in again, which should keep the same node; no key has expired on a board yet.

The node's identity is in NVS, which is not encrypted on ThistleOS: whoever has the flash has the device's tailnet identity, as they already have its WiFi passwords. Signing out erases it locally, and the node stays in the admin console until removed there.

While on, the client costs about 31 KB of internal RAM, most of it the stacks of its four tasks, and about 2 KB while off. Starting and stopping it run on a FreeRTOS task rather than a `std::thread`, which on ESP-IDF leaves about 150 bytes behind each time it ends.

Hermes, and any app after it, reaches the tailnet with ordinary sockets by name; the kernel offers no tailnet socket API. A packet that finds no WireGuard session is dropped while the session is made, so the first connect to a peer costs a TCP retransmit, about a second.
