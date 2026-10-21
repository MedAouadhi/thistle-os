# 06: GNSS on the T-Deck Max

**What to build:** The board acquires a position fix, and location-aware apps work.

The GNSS module is the same part the T-Deck Pro carries, so the driver is reused. Two things differ: the module's UART pins have moved — the pins the Pro used for GNSS are the Max's console UART — and its power rail is now behind the IO expander rather than on a native GPIO, so it must be raised during the expander output phase before the driver probes.

**Blocked by:** 03 (T-Deck Max boots to a usable system).

**Status:** in-review

- [x] The GNSS power rail is raised via an expander pin at boot, before drivers load
- [x] A driver entry declares the GNSS module at the Max's UART pins
- [ ] The module acquires a fix
- [x] Position is available through the GPS HAL interface

## Comments

The fix box stays unticked. The driver registers and starts cleanly on a real
Max, and the GPS HAL interface is live, but a fix needs the receiver enabled
and a view of the sky, and neither happened here. Enabling is a device-side
action — there is no GPS console command — so someone has to open Settings →
GPS → Enable, outdoors, and watch the satellite count climb.

### Bring-up found a bug the whole suite could not

Flashed to a T-Deck Max, the driver registered with the right numbers and then
refused to start:

    I (55866) board_builtin: gps-mia-m10q: uart=2 tx=16 rx=2 baud=38400
    GPS driver registered: MIA-M10Q
    ...
    Starting GPS...E (56086) uart: uart_param_config(973): not able to
    power down in light sleep
    GPS init failed (non-fatal)

`UartConfig` in the Rust driver is ESP-IDF's `uart_config_t` copied out by hand,
and it stopped one field short of it. Leaving the trailing `flags` bitfield off
did not make it go unread — `uart_param_config` reads `flags.allow_pd`
regardless, so it took whatever the stack happened to hold past the end of the
struct. The ESP32-S3 has no UART sleep retention, so a non-zero read there is
rejected outright rather than quietly ignored.

The bug is as old as the Rust port and affects every board carrying this
module, the Pro and the T-Deck included. Nothing had ever reached it, because
the builtin dispatcher had no case for this driver until this ticket added one:
the profile entry was parsed and skipped before it could fail. Adding the
registration is what exposed it.

Fixed in its own commit, with a `size_of` assertion alongside — nothing else
stops a hand-copied struct drifting when the header it mirrors grows a field,
which is exactly how the missing word got in. It holds at 32 bytes on Xtensa.

### What the hardware confirmed

Beyond the bug, the bring-up confirmed the parts a host test cannot:

- Detection picked the Max out by its expander, on a board with no profile:
  `I2C scan (SDA=13 SCL=14): TCA8418=1 CST328=1 BHI260=1 CST816=0 SSD1306=0
  CardKB=0 XL9555=1` → `Detected: tdeck-max`.
- The written profile carried the GNSS entry, and the dispatcher read it back
  with every value intact: `uart=2 tx=16 rx=2 baud=38400`.
- After the fix, `Starting GPS...` completes with no error.

One process note worth keeping. A flash alone would have shown none of this:
detection writes `/spiffs/config/board.json` only when it is absent, the
firmware build produces no SPIFFS image, and so the board kept the profile it
was holding from ticket 03 — which predates GNSS. The storage partition had to
be erased before the new profile could reach the board. This is the same trap
ticket 03 recorded, met a second time.

### What the board profile says, and where each number came from

Every value was taken from the vendor's board header and the schematic-derived
pinmap, and cross-checked against LilyGo's own GNSS bring-up sketch.

| Field | Value | Source |
| --- | --- | --- |
| Expander pin | 2, output, high | `BOARD_XL9555_02_GPS_EN (2)  // HIGH: enable GPS power` |
| UART | 2 | `#define SerialGPS Serial2` |
| `tx` (SoC side) | 16 | pinmap §9 "Module RX ← MCU TX … GPIO16" |
| `rx` (SoC side) | 2 | pinmap §9 "Module TX → MCU RX … GPIO2" |
| Baud | 38400 | `SerialGPS.begin(38400, …)` in `test_GPS.ino` |

The `tx`/`rx` pair is the trap here. These schematics name their nets from the
*module's* side — `GPS_TX` is the module transmitting, so it lands on the SoC's
**RX**. `uart_set_pin(port, tx, rx)` takes SoC-side pins, so the two are crossed
over relative to the net names. The dispatch block says so at the call site.

The rail goes up first in the expander phase and stays up. It is a rail, not a
reset: the keyboard and touch lines either side of it are pulses.

### Registration was the missing half

The board profile alone would not have been enough. `board_builtin_driver_init`
had no case for `com.thistle.drv.gps-mia-m10q`, so the entry would have parsed,
logged `no builtin for driver id`, and been skipped — the same silent shape the
rest of this effort keeps running into. The dispatch block is what actually puts
the GPS vtable in the HAL registry.

Enabling the receiver stays where it already was: `hal_registry_start_all` calls
`init`, and an app calls `enable` through the `thistle_gps_enable` syscall. The
settings app's GPS page does exactly that and reads position back, so the Max
inherits a working page rather than needing a new one. That is the existing
contract on every board with GNSS, not something this ticket changed.

### The module could be at 9600, and nothing would say so

Worth carrying into bring-up. The M10 leaves the factory at 38400; the M8 on
the older boards sat at 9600, which is what `tdeck-pro.json` still declares.
LilyGo's own sketch opens at 38400 and, if nothing answers, retries at 9600
before going back — they clearly expect to meet both.

The driver opens at whatever the profile says and does not go looking. So a
module that has been moved to the other rate presents as an idle line, and the
driver concludes there is no receiver fitted. If the Max shows no NMEA at all
on first power-up, try 9600 in the profile before suspecting the wiring.

Teaching the driver to probe both rates is real work — it needs a read loop that
can tell framing noise from NMEA, and hardware to validate against — so it is
recorded here rather than guessed at. `BOARD_GPS_PPS` (GPIO1) is likewise
undeclared: the driver has no PPS path, and the ticket does not ask for one.

### From review

A standards and spec review ran over the change. The spec axis found no
implementation errors: it re-derived every pin, the expander index, the UART
number, the baud rate and the tx/rx orientation from the vendor sources
independently and they all agree, and it confirmed the catalogue profile and the
embedded fallback profile were updated consistently.

Acted on:

- The baud-rate assertion's comment claimed the vendor "falls back to 9600" in a
  way that read as though this driver did too. It does not. The comment now says
  what actually happens, and the risk is recorded above.
- `the_tdeck_max_pulses_its_keyboard_and_touch_resets_low_then_high` asserts the
  whole expander sequence, so adding the GNSS rail to it left the name covering
  less than the assertion did. Renamed to say both things.
- Retiring the shadowed C implementation is in its own commit, so it can be
  taken or dropped separately from the bring-up.

Noted, not acted on:

- **`uart_num` is a port number where every other bus field is an index.** The
  glossary is explicit that a bus index is "distinct from the hardware host or
  port number", and this entry hands a raw peripheral number to the driver while
  the profile declares no `uart` bus array at all. True, and inherited verbatim
  from `tdeck.json`, `tdeck-pro.json`, `tdeck-plus.json` and
  `twatch-ultra.json` — the GNSS schema has always been shaped this way.
  Giving UARTs the bus-array treatment the SPI and I²C buses get is a board
  profile schema change touching five boards, not a rider on this one.
- **`tx` / `rx` would read better as `soc_tx` / `soc_rx`.** Agreed in the
  abstract, and the reason the comment has to exist at all. Renaming them for
  one board forks the schema the four other GNSS boards share; renaming them for
  all five is the same schema change as above.
- **The embedded fallback profile is hand-copied with nothing asserting the two
  agree.** Already covered: `validate_board_catalog.py` parses the profiles out
  of `board_fingerprint.c` and deep-compares each against the board profile file
  it duplicates, and CI runs it. It catches exactly this, and passing it is what
  forced the second copy of both new lines.

### Verified

- Kernel suite 1456/1456, up two from 1454.
- `validate_board_catalog.py` passes on all 14 boards, embedded profile in step.
- Simulator integration 18/18, with `radio_gps/tdeck-max` added alongside the
  `tdeck` and `tdeck-pro` cases it already ran.
- Firmware links: `thistle_os.bin` is 0x258f60 bytes, 6% free.
- `nm` on the ELF shows one `drv_gps_mia_m10q_get`, resolving to the Rust
  implementation, with the whole `mia_m10q` vtable alongside it.

Not verified: anything needing the board. No fix, no NMEA, no HAL read.

### Nothing ever started the receiver (2026-10-11)

Reported as "GPS not working". Two faults, neither in the wiring:

- **No app enabled it.** "An app calls `enable`" turned out to mean only the
  Settings toggle. Navigator just reads, so it never saw a sentence.
  `hal_registry_start_all` now calls `enable` once `init` succeeds. The rail is
  already up from the expander phase, so this only starts the reader task.
- **No fix was reported as an error.** `get_position` returned
  `ESP_ERR_INVALID_STATE` until a fix arrived, so Navigator showed "GPS error"
  and Settings "Status: Error" while the receiver was still acquiring. It now
  returns `ESP_OK` whenever the receiver is being read, with `fix_valid` telling
  the two states apart, as the simulator's driver always did. The contract is
  written down in `hal/gps.h`.

The driver also logs, once per `enable`, whether the line is live
(`gps: receiver talking on UART2 at 38400 baud`, or a warning with a byte count
after 5 s of silence), and when the first fix arrives.

On the bench, indoors: NMEA at 38400 on the first try, so the 9600 worry above
did not come up. UTC time was valid. GSV showed one or two satellites at
9–21 dB-Hz, never enough for a fix. The fix box stays open until someone takes
the board outside.

### Every sleep switched the receiver off for good (2026-10-20)

Reported as "never a fix, even outside". Two faults, the first enough on its
own.

**Light sleep left the receiver in a standby nothing woke it from.** Before
each light sleep (after 2 minutes idle on this board's settings, 5 by
default, or the power key) the driver sent UBX-RXM-PMREQ in its 8-byte form,
and woke the receiver after with one 0xFF. The 8-byte form is the M8's: the
receiver's boot banner says `ROM SPG 5.10`, `MOD=MIA-M10Q`, and the SPG 5.10
interface description (UBX-21035062) defines only the 16-byte form, with a
`wakeupSources` field, and the integration manual (§3.6.3.2) says standby
needs `force`. On the bench, over six runs, the first two from a receiver
power-cycled through XL9555 P02 so that its state was known:

| Sent | What the receiver did |
| --- | --- |
| 8-byte PMREQ | Silent after the 2 sentences in flight, 4 times; ran on, twice |
| then one 0xFF, the old wake | Stayed silent; so did three more 0xFF, and one 0x00 |
| then sixteen 0x00 | Woke, 4 times, on the first to the third try |
| 16-byte PMREQ, backup + force, uartrx | Silent after what was in flight, 12 times of 12 |
| then one 0xFF, one 0x00 or sixteen 0x00 | Talking 304 to 311 ms later, 12 times of 12 |

So from the first light sleep on, GPS was off until the board lost power or
deep-slept, which cuts P02; a restart does not. Settings showed "No fix,
Satellites: 0" throughout, as it does for a receiver that hears nothing.

The driver now sends the 16-byte form, and for a wake sixteen zero bytes,
which also wake a receiver an older firmware left in the other standby. The
reader sends them again every 3 s, up to ten times, until the receiver
talks. Waking no longer blocks the power manager for 500 ms. Switching GPS
off now sends the receiver to standby too, where it stops drawing tracking
current and keeps what it knows for a quick start; before, off only stopped
the reader. Sleeping or waking while off leaves it there. Enable flushes
the UART first, so a sentence from before standby is not read as current.

Checked on the board with a test build calling the driver's `sleep(true)`,
`sleep(false)`, `disable` and `enable` as the power manager and Settings do:
standby, a second `sleep(true)` sending nothing, waking in 306 ms, staying in
standby through a sleep and wake while off, and waking in 305 ms on enable.
Not checked: a real light sleep. It takes the board off USB and needs a key
to wake, and the board was on the bench unattended. The receiver's side is
the same; what that leaves open is the S3's TX pin during light sleep, which
would at worst wake the receiver early.

**Nothing showed what the receiver hears.** "Satellites" was GGA's satellites
in use, 0 until a fix whether the receiver needs a better view or hears
nothing. The driver now reads every constellation's GSV, and the GPS HAL has
`get_sky` (appended last, as `is_plugged_in` was): satellites in view, how
many are heard, and the strongest C/N0. Settings → GPS shows
`Satellites: 0 used, 4 in view` and `Signal: 4 heard, best 27 dB-Hz`, and
the log, at 30 s and gaps doubling to 10 minutes until the fix:

    gps: no fix after 30 s: 2 satellites in view, 2 heard, best 14 dB-Hz

then `fix after N s with M satellites, best signal X dB-Hz`. Indoors on the
bench it hears one to four satellites at 11 to 27 dB-Hz. A fix needs several
at around 30 or better.

### Still open

- The fix itself: outdoors, GPS on, the device awake (on a power bank, "Stay
  awake when plugged in"). If the signal row stays below about 30 dB-Hz with
  a clear sky, try again with mobile data off, to see if the modem is
  interfering, before suspecting the antenna. LilyGo's factory image in the
  vendor checkout is the hardware's reference.
- Saving the receiver's database (UBX-MGA-DBD) before deep sleep, which
  loses everything since P02 powers the whole receiver, and sending it back
  after: the ephemeris it holds is better than predicted orbits for 4 hours.

### AssistNow Predictive Orbits (2026-10-20)

u-blox's free assistance, now in `gps_assist.rs` (the kernel) and the
driver's new `assist` and `get_identity` (`hal/gps.h`). Sources: the
Thingstream AssistNow user guide, getting-started guide, service
integration guide, service specifications and host-software guide (read
through support.thingstream.io's help-centre API, as the pages refuse
scripted reads), the AssistNow API at assistnowapi.services.u-blox.com, the
M10 SPG 5.10 interface description (UBX-21035062: MGA-ACK, MGA-ANO,
MGA-INI-TIME_UTC, MGA-INI-POS_LLH, SEC-UNIQID, MON-VER, CFG-NAVSPG-ACKAIDING)
and the MIA-M10Q datasheet's table 5.

What the service is:

- Predictive Orbits is free and permanent; Live Orbits is paid. The old
  AssistNow Offline/Online tokens are no longer issued. Each receiver is
  registered once (zero-touch provisioning): a POST to
  `https://api.thingstream.io/ztp/assistnow/credentials` of a device
  profile's token and the receiver's UBX-SEC-UNIQID and UBX-MON-VER replies
  as hex, answered with a chipcode, a service URL and the data the profile
  allows. That last, `allowedData`, is a list in the guide but one string
  from the service: `"ualm, uporb_1, uporb_3, uporb_7, uporb_14, ulorb_l1,
  utime, ukion, usvht"` for an evaluation profile, which takes Live Orbits
  too and cannot be set to leave them out (2026-10-21). The data is then a
  GET of
  `GetAssistNowData.ashx?chipcode=...&gnss=gps,gal&data=uporb_7,ualm`.
- Limits: an evaluation profile, 300 requests a month a device, three
  devices an account; a commercial one, 2 a day. A 403 says the count is
  reached; it starts again at 00:00 UTC.
- The orbits come as UBX-MGA-ANO, one a satellite a day, and the almanacs as
  UBX-MGA-*-ALM. This receiver has no flash, so the host keeps them and sends
  only the day's (host-based, as the guide calls it). **The MIA-M10Q takes
  orbit predictions for GPS and GLONASS only** (datasheet table 5). The
  almanacs it takes for all.

What the device does:

- With a token (`ASSISTNOW_ZTP_TOKEN` in .env, built into the firmware;
  `ztp_token` in the card's `config/assistnow.json` wins, and
  `"enabled": false` there leaves the service and its data alone), it registers the receiver once
  GPS is on and there is a network and a clock, then fetches 7 days of GPS,
  Galileo and GLONASS orbits and the almanacs, 50 KB or so, once every 20
  hours, and at once if what it keeps was for other constellations. It
  sends no position: Predictive Orbits needs none. Failures wait 15 min,
  1 h, 4 h, then 12 h; a 401 registers again.
- `/spiffs/assistnow.json` keeps the chipcode, when the data came and the
  last fix (written at most every 30 minutes); `/spiffs/assistnow.ubx` the
  data. A week with no network still starts with orbits.
- Whenever the receiver starts (boot, GPS switched on, a wake from sleep),
  unless it had a fix in the last 2 hours, the kernel hands the driver the
  time, the last fix and the data; and again when new data comes while it
  has no fix, and when a much better time comes (below). The fix's
  accuracy is 1 km plus 100 km an hour since it was taken, left out past
  3000 km. A receiver told better than the truth starts worse than one told
  nothing (the interface description's warning on both messages).
- The driver keeps the almanacs and today's GPS and GLONASS orbits, and once
  the receiver talks sends CFG-NAVSPG-ACKAIDING (RAM), MGA-INI-TIME_UTC,
  MGA-INI-POS_LLH and those, 200 bytes a pass, about half the line. Each
  MGA-ACK is matched to its message by its ID and first four payload bytes;
  what went unanswered, or found the receiver not ready (infoCode 5), goes
  once more. The log then says `assistance: time taken, position taken, 31
  of 31 almanacs and 31 of 31 orbits taken`, and `assistance: N of M
  refused, the first for reason K` with any refusal.
- Settings → GPS has an Orbits row: `until 26 Oct`, `no AssistNow token`,
  `registering`, `downloading`, `failed, HTTP 403`.
- Privacy: the registration sends the receiver's unique ID and versions to
  Thingstream, and each fetch the chipcode, with the device's address;
  nothing else, and never a position.

Checked: 26 kernel tests (the frames, today's selection, the pacing, the
acknowledgements and the second try, the decisions, the time and position
accuracies, the registration's body and answer, the URL), and
`simulator/tests/test_gps_assist.py` against a fake Thingstream and
AssistNow: registration, fetch and hand-over; kept data asking nothing; no
token asking nothing; a 403 not asked again at once.

On the bench, 2026-10-21, with an evaluation profile's token on the card,
indoors:

| Step | Log |
|---|---|
| Boot, no orbits yet | `handed to the receiver: the time to 15 s, no position, no orbits` |
| The receiver | `assistance: time taken, position none, 0 of 0 almanacs and 0 of 0 orbits taken` |
| Registration | `this receiver is registered; orbits come 7 days at a time` |
| Fetch, GPS and Galileo | `7 days of orbits fetched: 469 messages, 37052 bytes` |
| Hand-over | `handed to the receiver: the time to 14 s, no position, 37052 bytes of orbits and almanacs` |
| The receiver | `assistance: time taken, position none, 55 of 55 almanacs and 32 of 32 orbits taken` |

The first registration was refused by the parser, not the service: the
guide's list was a string. The redacted answer it now logs on such a
failure is how that showed. Still to see: the fix outdoors with the orbits,
and a start with a last position.

To set it up: sign up at portal.thingstream.io; Services → ZTP → Device
Profiles → Create, module OTHER, assistance Predictive Orbits, an
Evaluation profile while trying it; copy its ZTP token into .env as
`ASSISTNOW_ZTP_TOKEN=...` and rebuild, or into the card's
`config/assistnow.json` as `{"ztp_token": "..."}`.

### GLONASS, and the time to a second (2026-10-21)

Outdoors the first fix with the orbits still took minutes. An IMU would not
have helped: SPG 5.10 has no sensor input (no UBX-ESF class), and dead
reckoning keeps a fix going rather than finding the first. Two changes
instead:

- **GLONASS on.** The module comes up with GPS, Galileo and BeiDou B1I,
  and B1I cannot run with GLONASS (integration manual §2.1.3). The driver
  sends, once the receiver talks after each wake, a UBX-CFG-VALSET to RAM
  and BBR: GLO_ENA, GLO_L1_ENA, BDS_ENA and BDS_B1C_ENA on, BDS_B1_ENA
  off, the four-constellation set the manual names; B1C acquires a little
  less readily than B1I. A change restarts the GNSS subsystem, so the rest
  waits 1 s (the manual asks for the acknowledgement and 0.5 s). The log
  says `tracking GPS, Galileo, GLONASS and BeiDou B1C` once a boot, or
  `the receiver refused ...` on a UBX-ACK-NAK. AssistNow is asked for
  `gps,gal,glo` (BeiDou's predictions would be 23 KB a week it does not
  take), and kept data for other constellations is fetched again.
- **The time.** It was 10 s plus 0.5 % of the time since the clock's last
  sync. It is now the clock plus what is still to be slewed in (the time
  its source gave), good to the source's own accuracy (NTP 0.1 s; GPS, the
  mobile network and a clock chip 2 s; by hand 60 s), plus 0.5 s for the
  hand-over, plus 50 ppm of the time awake and 1 % of the time asleep
  since that sync. The power manager counts the time in light sleep
  (`power_manager::slept()`); a sync before the last wake, or before this
  boot, counts all its time as asleep. The boot's hand-over usually comes
  before NTP; a time at least twice as good, where the first was over 3 s,
  is handed over again while there is no fix.

On the bench, indoors, after the reflash:

| Step | Log |
|---|---|
| Signals | `tracking GPS, Galileo, GLONASS and BeiDou B1C` |
| Boot hand-over, the kept data | `the time to 12 s, its last fix to 32 km, 37052 bytes` |
| NTP | `clock slewing by -1418 ms, from NTP` |
| Fetched again, for GLONASS | `7 days of orbits fetched: 640 messages, 50456 bytes` |
| Handed over again | `the time to 1 s, its last fix to 32 km, 50456 bytes` |
| The receiver | `time taken, position taken, 79 of 79 almanacs and 53 of 53 orbits taken` |
| The sky | `38 satellites in view, 8 heard`, where it was 13 in view |

The 12 s was honest: the clock was 1.4 s out. Still to see: the first fix
outdoors with all of it.

### AssistNow Autonomous, and a report of each start (2026-10-21)

On the balcony the first fix still took minutes: many satellites heard,
some used only much later. The likely reasons are the orbits and the
sleep, and a report on the card is there to tell which:

- **Orbits.** The receiver takes AssistNow's predictions for GPS and
  GLONASS only (datasheet table 5), so Galileo and BeiDou wait for an
  ephemeris read off the satellite: 30 s of clean signal for GPS's, longer
  for the others, and a balcony's signal is weak and reflected. **AssistNow
  Autonomous** is now on: CFG-ANA-USE_ANA (0x10230001), sent in RAM and
  BBR after the settle at each wake, in a UBX-CFG-VALSET of its own so a
  refusal would not take the signals with it. The receiver then extends
  each ephemeris it decodes into about three days of its own predictions
  (integration manual §3.12.4) for GPS, Galileo and BeiDou; GLONASS would
  need a flash this module does not have. The manual recommends it beside
  AssistNow Offline: the receiver picks the better prediction for each
  satellite. BBR keeps them through standby, not deep sleep. The log says
  `AssistNow Autonomous on: the receiver predicts the orbits it decodes`
  once a boot, or `the receiver refused AssistNow Autonomous` on a NAK.
  The driver now matches each UBX-ACK to the VALSET it answers, in order.
- **Sleep.** Nothing holds the device awake while GPS looks for its first
  fix. After the idle time (120 s on this board) it light-sleeps, the
  receiver goes to standby, and the search starts again at the next wake.
  A report ended `by a sleep` shows it.

**The report** (`gps_report.rs`): one file a start on the card,
`gps/starts/NNNN-YYYYMMDD-HHMMSS.jsonl` (UTC; `NNNN.jsonl` without the
clock), JSON lines: `start`, `handed` (the time's and the position's
accuracy, the bytes), `sats` every 5 s from UBX-NAV-SAT, `fix`, and `end`
with why and a summary per constellation. It ends 30 s after the fix, at
the receiver's next start (`by a sleep` or `by GPS switched on again`),
after 30 minutes without a fix, or at 2 MB. No position is written, the
last 20 are kept, and `"report": false` in the card's config/assistnow.json
stops it. The satellites come from the driver's new `get_satellites`
(`hal/gps.h`): each call polls UBX-NAV-SAT, read whole off the wire (up to
1024 bytes, 84 satellites, where the frame buffer was 400), and returns the
last one with its age. A satellite's row is `["G12", dB-Hz, elevation,
azimuth, quality, used, health, orbit it is navigated by, orbits held]`,
the orbits `eph` (decoded), `alm`, `pred` (AssistNow's), `auto` (its own).
The log has the summary of the first `sats` and of the `end`.

On the bench, indoors, after the reflash:

| Step | Log |
|---|---|
| Autonomous | `AssistNow Autonomous on: the receiver predicts the orbits it decodes` |
| The report | `reporting this GPS start in gps/starts/0001-20261021-100004.jsonl` |
| Its first look, at 1 s | `GPS 1 heard, 0 used, orbits: 4 decoded, 11 predicted, 3 autonomous; Galileo 0 heard, 0 used, orbits: 3 decoded, 0 predicted, 3 autonomous; GLONASS 1 heard, 0 used, orbits: 0 decoded, 8 predicted, 0 autonomous; BeiDou 0 heard, 0 used, orbits: 3 decoded, 0 predicted, 3 autonomous` |

Still to see: a balcony start's report.
