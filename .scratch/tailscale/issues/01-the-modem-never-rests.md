# 01: The modem never rests while the tailnet is on

Status: in-review

**What to build:** While the device is awake on mobile data with Tailscale
on, MicroLink sends something every few seconds, so the A7682E never drops
back to idle between one burst and the next. An idle tailnet should be
quiet: send what keeps the device reachable and online, at the rate
Tailscale's own clients do, and nothing for its own sake.

- [x] A test build that counts what the tailnet sends, per minute, idle on
      LTE with Hermes closed: before, and after the changes below
- [x] Control plane: no HTTP/2 PING every 5 s, and a watchdog fed by what
      the server sends, not by what the device sends
- [x] STUN: while a peer's session is in use, and only every few minutes
      otherwise; an endpoint update only when the endpoints change
- [x] DISCO: direct-path probes only to a peer with a session in use, not
      to every peer every 15 s
- [x] DERP: NotePreferred when the home region is chosen or changes, not
      every 60 s
- [ ] Nothing lost: from idle, Hermes is connected within 2 s (done); after
      10 minutes idle a peer's ping is answered (done); a peer added in the
      admin console reaches the device; the admin console shows the device
      online (done, as another node's `tailscale status` sees it); on WiFi
      a session still goes direct (done)
- [ ] The battery's current, idle on LTE: tailnet off, on before, on after

**Blocked by:** nothing

## What is sent, idle on LTE

As of MicroLink `6baf8a4` (`components/microlink/` in the fork), with no
WireGuard session in use and the device awake:

| Every | What | Where |
|---|---|---|
| 5 s | HTTP/2 PING to the control server | `src/ml_coord.c:2919` |
| 23 s | STUN, to the home DERP region's STUN server, IPv4 and IPv6 if known | `src/ml_coord.c:2853`, `ML_STUN_RESTUN_INTERVAL_MS` (`include/microlink_internal.h:103`) |
| 23 s | An endpoint update to the control server after each STUN answer, a 455-byte MapRequest, changed or not | `src/ml_coord.c:2821`, `do_send_endpoint_update()` at 2092 |
| 15 s, per peer | A DISCO ping to every IPv4 endpoint the peer advertises, and one through DERP; the peer answers through DERP | `disco_periodic_probes()`, `src/ml_wg_mgr.c:1457`; `disco_send_ping_to_peer()` at 776; `ML_DISCO_UPGRADE_INTERVAL_MS` (`microlink_internal.h:89`) |
| 60 s | DERP NotePreferred | `src/ml_coord.c:2878` |

Seen on the T-Deck Max, 2026-10-17, idle on LTE for 412 s: 18 endpoint
updates, one every 23 s, and 8 MapResponses from the control server, about
one every 52 s (presumably the keep-alives MicroLink asks for with
`KeepAlive: true`; their content was not logged). The other
senders log at INFO under tags ThistleOS turns down to WARN, so they were
not counted; their intervals are the code's.

Notes on each:

- **The 5 s PING** is justified in its comment by "a ~20s idle timeout" on
  the server, "(v1 reference)". Nothing here confirms it, and the server
  appears to send something about once a minute anyway. The PING also resets
  the control watchdog (`last_activity_ms = now` at `ml_coord.c:2937`;
  `ctrl_watchdog_ms`, 120 s by default), so the watchdog cannot notice a
  dead server while sends still succeed. Received frames alone should feed
  it.
- **STUN and the endpoint update** go together: each answer sends an
  update. Whether the answer differs from one STUN to the next on LTE was
  not recorded. If the carrier's NAT gives each probe a new port, "only
  when changed" would still send every time, and the port churn of a NAT
  that cannot be reached directly anyway should not count as a change. On
  WiFi the log said `Cone NAT: port 51820 consistent`.
- **DISCO's probes** look for a direct path to every peer, used or not.
  MicroLink skips them only in its own AT-socket cellular mode
  (`ml_at_socket_is_ready()`), which ThistleOS does not use: its LTE is
  esp_modem's PPP. Skipping them on cellular altogether is not the answer:
  a peer with a public address can sometimes be reached directly through
  a carrier's NAT. Probing only peers with a session in use is.
- **The direct-path heartbeat**, every 3 s (`ml_wg_mgr.c:1469`), runs only
  while a direct path is trusted, so on WiFi; same pattern, smaller cost.
- **No longer sent**: WireGuard keepalives and renewals (`6baf8a4`; idle
  sessions now lapse). The DERP task's 5 s `HEARTBEAT` is a log line.
- **From the servers**, the control keep-alive and DERP's, arrive whatever
  the device does while it holds those connections. They set the floor.

## Why it matters

A7682E Hardware Design V1.05, table 35 (VBAT 3.8 V): LTE sleep 2 mA, idle
12 mA, LTE data at 23 dBm 534 mA (B5) to 837 mA (B7). After a transfer the
modem stays connected until the network's inactivity timer, which the
carrier sets, runs out; a send every 5 s means it never does. The current
while connected and quiet is not in the datasheet: measure it. The cell is
1400 mAh (tdeck-max ticket 13).

This costs only while the device is awake. While it sleeps the network
rests (`net_coordinator.rs`, `rest`): the data session ends and the modem
sleeps listening for calls and SMS. So the cost is the time the device is
in use, held awake (a Hermes turn holds it up to ten minutes), or plugged
in with "Stay awake when plugged in".

## What Tailscale's own client does

To be checked in Tailscale's source (`control/controlclient`,
`wgengine/magicsock`, `derp/derphttp`) before copying, as the intervals
here are from memory:

- the control long-poll asks for the server's keep-alives and watches for
  them, rather than pinging every few seconds;
- magicsock re-STUNs periodically only while it has been active recently,
  and reports endpoints to the control server when they change;
- DISCO pings a peer while traffic flows to it, with a heartbeat while the
  session is active and upgrade attempts on a longer interval;
- NotePreferred is sent when the DERP connection is made or the preference
  changes.

## How to measure

- **Counts.** A test build, not kept, with `ml_coord`, `ml_stun`,
  `ml_wg_mgr` and `ml_derp` at INFO (`quiet_the_client()` in
  `components/kernel/src/tailnet_platform.c`). `ml_wg_mgr` at INFO also
  logs its periodic work every 400 ms and every second; filter those out.
  Count each sender per minute over at least 5 minutes idle, Hermes closed.
- **Current.** The BQ27220's current (`GAUGE_CMD_CURRENT` in
  `components/kernel_rs/src/drv_power_sy6970.rs`) is read only for the
  boot log line. Sample it every few seconds and show it on screen or write
  it to the card: on USB the gauge reads the charge current, so the test
  runs on battery, where the console is gone. Set sleep to never (Settings
  → Power, 0) for the test. Ten minutes each, screen untouched: LTE with
  the tailnet off, on as now, on after.

## Where the changes go

In the fork, `MedAouadhi/microlink` branch `thistle`, pinned by commit in
`components/kernel/idf_component.yml`; move the pin as CLAUDE.md says
(delete `dependencies.lock`, put back what else moves). The STUN interval
and the watchdog can be set from `tailnet_platform.c` without a fork
commit (`microlink_config_t.stun_interval_ms`, `.ctrl_watchdog_ms`), but
the PING, the endpoint update rule, DISCO's probes and NotePreferred need
commits in the fork.

## Related, not this ticket

- **Every wake restarts the tailnet** over LTE: the Noise handshake,
  registration, a 21 KB map (`MapResponse recv+parse: 1408 ms ... 21KB`),
  DERP's TLS and STUN, and once an hour (`DERP_PROBE_EVERY_MS`,
  `ml_coord.c:2343`) a TCP connect to each DERP region to pick the
  nearest (the map had 28). A cost per wake rather than per minute.

## Comments

2026-10-17, MicroLink `709ba05`, checked against Tailscale's own source
(tailscale.com v1.100.0) before copying:

- `control/controlclient/direct.go`: no PING of its own; a 120 s watchdog
  (`watchdogTimeout`) re-armed by each message, keep-alives "every minute".
- `wgengine/magicsock`: re-STUN every 20 to 26 s only while the tunnel
  carried a packet in the last 45 s (`sessionActiveTimeout`), and none at
  all otherwise; endpoints sent only when the set changes, ports included.
  DISCO pings a peer only while sending to it (heartbeat every 3 s,
  stopping 45 s after the last send), or in answer to a call-me-maybe.
- `derp`: NotePreferred on connect and when the preference changes; the
  server's KeepAlive every 60 to 65 s; the client reads with a 120 s
  deadline.

What changed is in the fork commit's message. Two choices beyond copying:
STUN every 5 minutes while idle, as this ticket asked, where tailscaled
stops; and a STUN port that moved alone is news only while a session is
in use and the NAT maps one port for every destination. Idle on LTE each
5-minute probe was given a new port (63456, 63493, 63327) on one address,
which would otherwise have sent an update each time; the carrier's NAT
read as cone on one boot and symmetric on the next.

Counts, T-Deck Max on USB (so awake), LTE, WiFi off, Hermes closed, from
2 minutes after boot, with `measure/microlink-count.patch` applied and
`measure/count_tx.py`:

| Sent, per minute | `6baf8a4`, 7.3 min | `709ba05`, 10.3 min |
|---|---|---|
| DISCO, direct UDP | 103 | 0 |
| DISCO, through DERP | 35.7 | 0 |
| HTTP/2 PING | 11.9 | 0 |
| STUN | 2.6 | 0.19 |
| Endpoint update | 2.6 | 0 |
| DERP NotePreferred | 1.1 | 0 |
| WINDOW_UPDATE, answering a keep-alive | 1.1 | 1.16 |
| All | 158 | 1.4 |
| Bursts (packets under 2 s apart) | 11.6 | 1.35 |
| Longest gap between bursts | 5.0 s | 57 s |

Received, after: the control plane's keep-alive 1.16 a minute and DERP's
0.97, never in step, so the modem still wakes about twice a minute; that
is the floor while both connections are held. Before, DERP also sent back
24 PeerGone a minute, for DISCO through DERP to peers that were offline.

While a peer uses the session (the laptop's `tailscale ping`, then ICMP)
the device probes for a direct path, about 150 UDP packets a minute
through LTE to the laptop's advertised endpoints, mostly in answer to its
call-me-maybe every 5 s, and STUN goes every 23 s. 45 s after the last
packet it is back to 1.7 a minute.

Nothing lost, same setup:

- The control plane showed the device online at 2, 5, 8 and 11 minutes
  (`tailscale status --json` on the laptop: `Online true`).
- After 12 minutes idle: `tailscale ping` 3/3 through DERP (fra), 167 to
  188 ms; ICMP 5/5, 395 ms for the first (starting a session), then 113 to
  134 ms.
- Hermes from idle on LTE: connected 1.06 s after it started, session up
  at 1.28 s. On WiFi, after the tailnet restarted for it: 0.71 s and
  0.74 s.
- On WiFi, the laptop's first `tailscale ping` went direct
  (192.168.2.230:51820) and ICMP took 9 ms at best.
- Both watchdogs set to 30 s in a test build: each fired every half
  minute, the control connection registered again and DERP reconnected
  (sending NotePreferred as it did) every time, and the device stayed
  reachable. At 120 s, neither fired in 16 minutes.

Still to do:

- **The battery's current.** `measure/current-logger.patch` (thistle-os,
  not to be committed) writes the gauge's current once a second to
  `/sdcard/current.csv`, `uptime_s,current_ma,millivolts`, negative on
  battery; build it once on `6baf8a4` and once on `709ba05`, without the
  count patch. Sleep set to never, unplugged, ten minutes each, screen
  untouched: tailnet off, on with each build.
- **A peer added in the admin console** reaching the device: not tried.
