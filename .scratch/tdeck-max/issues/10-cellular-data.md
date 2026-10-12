# 10: Mobile data on the T-Deck Max

**What to build:** With a SIM fitted and mobile data switched on in Settings, the board reaches the internet over LTE. Traffic takes WiFi whenever WiFi is connected and LTE otherwise, with no app aware of which.

The spec deferred the A7682E: "Cellular data is a subsystem, not a driver." This ticket is that subsystem. It is a driver entry that registers a network transport, plus a kernel coordinator that keeps the transports up. The routing decision is ADR-0003.

**Blocked by:** 02 (IO expander phase), 03 (T-Deck Max boots to a usable system).

**Status:** in-review

## What the hardware is

From the vendor checkout (`~/personal/T-Deck-MAX`), `docs/pinmap.md` section 10 and the schematic:

| Line | Where | Notes |
| --- | --- | --- |
| Module supply | XL9555 P00, `6609_EN` | High powers the SGM6609 boost that feeds the module's VBAT |
| PWRKEY | XL9555 P10, `PWRKEY_EN` (expander pin 8) | Drives Q5, an MMBT3904, through R55 (schematic page 5). **High presses the key** |
| SoC TX → module RXD | GPIO10 (`7682_RXD`) | Net named from the module's side |
| SoC RX ← module TXD | GPIO11 (`7682_TXD`) | Likewise; the vendor opens `Serial1.begin(115200, ..., rx=11, tx=10)` |
| DTR | GPIO8 | Held low |
| RI | GPIO7 | Not used |

The vendor's sketches hold PWRKEY_EN high for good: `test_AT.ino` writes it high once, and `factory.ino` drives every expander pin high. The factory sketch's own `digitalWrite(BOARD_A7682E_PWRKEY, ...)` pulse passes the expander pin number, 8, to the SoC's `digitalWrite`, so it would land on GPIO8, which is DTR. The driver pulses the key instead, for 100 ms against the 50 ms typical, and then releases it. A released key is what AT+CPOF needs: shut down with PWRKEY low, the module powers itself on again (A7682E Hardware Design V1.05, 3.2.1).

## On hardware

T-Deck Max on /dev/ttyACM0, no SIM fitted.

- **The board held a `board.json` from before this ticket**, the same trap as tickets 03, 06 and 09. Instead of erasing the storage partition, a temporary build re-ran detection once, which rewrote only `/spiffs/config/board.json` from the embedded profile. `system.json`, the mesh identity and the rest of SPIFFS were kept. Then: `board_builtin: builtin: com.thistle.drv.modem-a7682e (hal=network)`, `Registered transport: LTE`.
- **Power-on**, with mobile data forced on by a temporary build. At 9.2 s the UART opened. Neither an AT probe nor a `+++` escape got an answer. At 15.6 s: `pressing PWRKEY for 100 ms`. At 23.2 s: `modem answering`, 7.6 s later (8 s typical).
- **SIM.** The first answer, 10 ms after the module answered, was `+CME ERROR: SIM failure`. The next was `+CPIN: SIM REMOVED` / `+CME ERROR: SIM not inserted`. So a verdict other than "ready" is only taken once it has held for four answers. With that in place the attempt ended `SIM not usable (state 2)` (absent), then `AT+CPOF accepted`, `power-off: done` 3 s later, supply cut.
- **Default boot** (mobile data off): the coordinator opens the UART, finds nothing answering, and cuts the supply: `mobile data: modem off` at 14.4 s, in the background. WiFi joined as before.

## With a SIM

MEDIONmobile SIM, no PIN, APN left to the network.

- **Mobile data on at boot.** Registered 4 s after the module answered, at -69 dBm, and had an address (10.191.179.175) 37 s after boot.
- **Switched on from Settings, it never connected.** That was the first report, "Status: Not connected". The log said `E pthread: Failed to create task!` every 5 s. By then internal RAM had 24 KB free, its largest block 7 KB, and the attempt's thread wanted 8 KB. At boot the UI has not taken its share yet, which is why the boot attempt worked. PSRAM had 8 MB free. The modem's worker threads then took PSRAM stacks, and WiFi rejoining, which needs internal RAM, ran on the coordinator's own thread. Ticket 11 replaced both: everything now runs on the coordinator's thread, whose stack is internal, with 180 KB of internal RAM free. A thread that cannot be started now counts as a failed attempt and backs off, instead of being retried silently every tick. After the fix, switching on at 54 s gave an address at 78 s.
- **Routing, by HTTP to example.com from a test build.** With both links up: `default netif sta`, 200 OK in 44-49 ms. With WiFi switched off: `default netif ppp`, 200 OK four times in 122-244 ms, DNS included. With WiFi switched back on, the coordinator rejoined it and traffic went back to it.
- **A session ended by the other side.** In one run, the session opened at 77.5 s ended at 83.8 s (`ppp: Connection lost`, lwIP's PPPERR_CONNECT: the module or the network hung up). It was not seen again in four other sessions, which ran two minutes or more. The next attempt then waited 15 s for an SMS redial that was not happening. It now waits only when an SMS exchange did redial, and it logs the module's `AT+CEER` answer for a session that ended on its own.
- **Reset with a session up.** The module stays in its data session. The next boot escaped it (`+++`, ATH) and had a new address 6 s after the driver found the module. Later runs showed that this does not hold up; see the UART rate below. Such a module is now restarted.

## The UART rate

The module's UART takes up to 3686400 baud, but no RTS/CTS is wired and the lines pass the RS0104 level translator (U17). The profile now sets `fast_baud_rate: 921600`. The driver powers the module up at 115200 and switches both ends with `AT+IPR` once it answers. It looks for a module left running at either rate.

Measured with Wi-Fi off, downloading 400 KB and uploading 150 KB to speedtest.tele2.net:

| Rate | Download | Upload |
| --- | --- | --- |
| 115200 | 9.7-9.8 KB/s | 10.7-10.8 KB/s |
| 921600 | 42-57 KB/s, about 50 typical | about 50 KB/s |

Three soaks at 921600 each lost the session once to the LCP keepalive (`ppp: Connection timeout`). A temporary build traced the PPP link at the UART. It wrapped `uart_read_bytes` and `uart_write_bytes`, decoded HDLC in each direction, and logged once a second with lwIP's link counters. Over about 6 MB:

- No frame had a bad FCS, there were no framing errors and no FIFO overflows, and lwIP dropped nothing. The link does not corrupt anything at 921600.
- In a drop, nothing arrived from the module for 26 s. The SoC's receive buffer was empty, and its echo requests and then Terminate-Requests kept going out. Then the module answered `+++` and AT at once, at 921600, and the redial got the same address. `AT+CEER` said `"0 Unknown"`.
- Every stall followed a boot that took over a session the module kept through the SoC's reset. One such boot needed three dials and 117 s: the module ignored our first Configure-Request, answered nothing for 30 s on another attempt, and then answered everything 1 ms after our next frame. The 115200 control run, which also began by taking over a session, spent 24 s bringing the link up and then got no address. So the takeover is the trouble, not the rate.
- Started afresh (mobile data off, which sends AT+CPOF and cuts the supply, then on), the module ran 17 minutes of six transfer bursts and two-minute idles with no stall.

So `a7682e_power_on_locked` now shuts down a module it found only by escaping a session, waits 2 s (Toff-on, A7682E Hardware Design V1.05), and starts it with PWRKEY. On two boots after a reset in session, LTE was up 27 s after the coordinator started. The two boots then ran 17 and 5 minutes of the same cycles at 921600 with every keepalive answered.

Also seen:

- In three of five starts from power-off, the module ended the first session within a second of opening it: an LCP Terminate-Request, then a `+CGEV` report. lwIP reported it 6 s later as `Connection lost`, and the redial had a new address within 20 s. See the first dial below.

## The first dial after a power-on

A test build switched mobile data off and on, which is AT+CPOF, supply cut and a fresh start, nine times in a row. Each first session was watched for 25 s.

- **As it was:** 8 of 9 first dials failed. Four went silent and timed out; four connected after 3.5-5 s and were ended by the module about 7 s later. Every second dial connected in 0.3 s.
- **Waiting for context 1 first** (`AT+CGACT?`) changed nothing. Context 1, the default bearer, was active the moment the module registered, alongside the IMS bearer in context 8.
- **Waiting 20 s after registration** changed nothing either: 8 of 8 first dials failed. `PB DONE` came 8 s after registration.
- **The cause was the dial itself.** After attach, `AT+CGDCONT?` reports context 1 with the APN the network gave it (`internet.mnc003.mcc262.gprs` here) and its address. esp_modem writes `AT+CGDCONT=1,"IP","<apn>"` before `ATD*99#`, and with the empty APN Settings defaults to, that rewrote the active default bearer's definition. Written back with the APN the module reports, 26 starts in a row connected on the first dial, 0.12-0.35 s after dialling, and every session held. The first one kept the attach's own address, so the bearer is reused, not rebuilt.
- **The cost:** AT+CGDCONT is saved, so the module now asks for that APN at every attach. Writing an empty APN back before AT+CPOF answers OK, but the module goes on reporting the network's APN. A SIM from another network may need its APN typed into Settings, if that network turns down the old one. That is untested.

Also seen, once in the 13 off/on cycles of one run: `ESP_MODEM_THROW: esp_modem_uart.cpp:28 create uart event task failed`, then `abort()` and a reboot. Each power-on destroys and re-creates esp_modem's DCE, and with it a 4 KB task in internal RAM; that time there was no room. esp_modem throws, and with C++ exceptions off a throw aborts.
- One download stopped for 28 s while the link was healthy, with every keepalive answered within 1 ms. The data stopped upstream of the module.
- Internal RAM fell to 655 bytes free at its lowest during transfers, though lwIP reported no failed allocation.

## Review

A review of the change found, and this ticket fixed:

- **A session ended by the LCP keepalive went unnoticed.** esp-idf posts `IP_EVENT_PPP_LOST_IP` only for `PPPERR_CONNECT`. A peer that stops answering (`PPPERR_PEERDEAD`) shows up only as a `NETIF_PPP_STATUS` error, which the driver now listens to.
- **SMS during a data session.** Sending one leaves data mode and redials. The coordinator saw the session drop and started over. A connect attempt now waits for that redial, and no AT command is sent while the DCE is in data mode.
- **Superseded attempts.** An attempt that finished after mobile data was switched off left the modem on. One that found no SIM after an off/on toggle blocked the new attempt. Attempts now carry the epoch they started in.
- **Smaller fixes:** SMS settings are re-applied after a power cycle. The SMS poll task now ends itself rather than being deleted mid-callback. A modem known to be off is not probed again. A modem that stops answering gets PWRKEY before its supply is cut.

Left open: incoming SMS are not received (see ADR-0003); CMUX is the fix.

## Still to see

- A SIM from another network, now that the module keeps this network's APN.
- The abort when esp_modem cannot start its UART task. Ticket 11 left about 180 KB of internal RAM free, against 14 KB, so the 4 KB it asks for is there. esp_modem still aborts rather than failing if it is not.
- Failover when WiFi is lost rather than switched off: walking out of range, or the router going down.
- A SIM that needs a typed APN.
