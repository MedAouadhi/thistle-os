# Tailscale

Status: built; on the T-Deck Max it signs in, rejoins, and reaches peers both ways

## Problem Statement

The device should reach the user's own services, a Hermes agent first among
them, from wherever it is, without opening them to the internet. The user
already has a tailnet, which their laptop, phone and home servers are on.
The device has WiFi and, on the T-Deck Max, mobile data, but no way onto it.

## Solution

The kernel joins the tailnet while the user has it switched on: the
Tailscale app, its tile in the quick panel, or `tailscale up` in the
Terminal. Once connected:

- any socket reaches a peer's 100.x.y.z address through WireGuard;
- `getaddrinfo("homelab")` and `getaddrinfo("homelab.tail1234.ts.net")`
  answer from the tailnet's map, before DNS;
- traffic for the internet does not go through it.

The first time, the app shows a QR code for a sign-in URL; the user scans
it, signs in to Tailscale on their phone, and the device joins as their
device. From then on it rejoins by itself, after a restart, a sleep or a
change of network. The app lists the peers and has Switch off and Sign out;
the Terminal has `tailscale status|peers|up|down|ping NAME [PORT]`.

## Security

- **Nothing secret in the image or on the card.** The sign-in is
  Tailscale's own interactive login: the device holds a URL, and the user
  authenticates on their phone. MicroLink's build-time auth key, WiFi
  credentials and HTTP configuration server are all pinned off.
- **The device's identity is its own keys**, MicroLink's machine, node and
  disco keys in NVS. NVS is not encrypted on ThistleOS, as for the WiFi
  passwords: whoever has the flash has the identity. Sign out erases them;
  the node stays in the admin console until removed there.
- **Key expiry** is the tailnet's: a user's device expires after 180 days
  unless disabled in the admin console. The client then makes a new node
  key and asks for a sign-in again.
- **DERP's TLS is not verified** (MicroLink's `MBEDTLS_SSL_VERIFY_NONE`),
  as in hermes_esp32: DERP relays WireGuard packets, which are encrypted end
  to end, so its TLS is not what protects them. The control plane is
  reached over Noise, keyed to the server's public key.

## Decisions

- **MicroLink, forked**, over tailscale-mpe-rust (no licence, no netif,
  136 KB of stacks) and over upstream MicroLink (lacks the fork's repairs).
  ADR-0009. Six commits on branch `thistle` of `MedAouadhi/microlink`:
  mbedTLS 3; a stop that waits for its tasks and frees what they held; sign
  in by URL and say why a registration was refused; the nearest DERP region
  as home; the netif torn down on the TCPIP thread; a session started for a
  packet that finds none.
- **Not a `hal_net_driver_t`.** The net manager's VPN transport means a
  VPN that carries everything; the tailnet does not, and the status bar
  would name it for traffic it never sees.
- **Restarted on every change of underlay**, rather than left to reconnect
  by itself, so its sockets never sit on an interface that has gone, and
  it is back as soon as the network is.
- **Start and stop on a FreeRTOS task.** A `std::thread` on ESP-IDF leaves
  about 150 bytes of internal RAM behind each time it ends (a heap trace
  showed its `Thread` handle, condvar and name), and the client restarts at
  every sleep.
- **An auth key in `tailscale.key`** was built, tested in the simulator,
  and taken out on the owner's call, to keep one way in for now.

## Out of scope

- An exit node, or routing internet traffic through the tailnet.
- Taildrop, Tailscale SSH, Funnel, subnet routes, MagicDNS for names the
  map does not hold (`100.100.100.100` is not served).
- Peers' presence: MicroLink does not keep the map's `Online` field, so the
  app lists every peer, online or not.
- IPv6 inside the tailnet.
- The File Transfer app over the tailnet: it closes any connection that did
  not come in on the WiFi address.

## What the hardware has confirmed (T-Deck Max, 2026-10-15)

- [x] Sign in by QR code from the app, after switching on from the quick
      panel: joined as the user's device `thistle-xxxxxx`, key expiry
      2027-04-13.
- [x] A restart rejoins with the stored identity, no sign-in.
- [x] Home DERP region measured: Frankfurt (4), 19 to 33 ms, in 1.4 to
      2.8 s, over Dallas before; `tailscale status` on a peer shows `fra`.
      Measured once a boot, then reused for an hour ("measured 2 min ago").
- [x] The laptop reaches the device: `tailscale ping` direct over the LAN,
      ICMP ping 5/5.
- [x] The device reaches a laptop on the same LAN by name over TCP
      (`tailscale ping LAPTOP 18080`): 907 to 944 ms the first time, which
      starts the WireGuard session, then 7 to 35 ms.
      Before the fork's last commits it never answered: a packet that found
      no session was dropped and none was started.
- [x] Switching off and on, five times and then three: each reconnects
      and reaches the laptop again, and internal RAM with it off stays
      within 44 bytes (156667 each time; 153971, 153971, 154015). A heap
      trace over a full cycle records nothing left behind.
- [x] Once, the control plane closed the first long poll ten seconds after
      a boot, and the client reconnected within a second; the next boot
      did not do it.
- [x] Internal RAM: about 110 KB free while connected, about 141 KB at
      boot with it off.
- [x] Idle, it sends next to nothing (MicroLink 709ba05, 2026-10-17): on
      LTE 1.4 packets a minute, where it sent 158, mostly DISCO to every
      peer and a PING every 5 s; it stays online, a peer's ping is
      answered after 12 minutes idle, and Hermes connects in 1.3 s.
      Ticket 01.

## What the hardware has to confirm

- [x] Over mobile data, with WiFi off: joins, and relays through fra.
      Only since MicroLink 1bceac7 (2026-10-17): before it, with no
      session the WireGuard netif went link-down and a socket's packets
      left by the modem, and a session a peer started sent its data to the
      peer's advertised address, not DERP. Now a connect from a fresh start
      is up in 1.3 s, and a peer's pings are answered.
- [ ] A switch between WiFi and mobile data, and a light sleep and wake:
      back on the tailnet within seconds.
- [ ] Waiting for an admin's approval, on a tailnet that requires it.
- [ ] Sign out on the device, then signing in again by QR code.
- [ ] A node key's expiry (or one expired from the admin console): the
      device asks for a sign-in and keeps the same node.
- [ ] `esp_http_client` to a peer by name, a whole request rather than a
      connect, as Hermes will use it.
- [ ] Battery: MicroLink's tasks poll every 1 to 10 ms, so the CPU seldom
      idles while it is on. Not yet measured.
