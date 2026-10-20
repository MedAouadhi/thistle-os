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
- Assistance (AssistNow): Predictive Orbits is free for M10 once a device is
  registered on Thingstream, Live Orbits is paid. With no account, time and
  last-position aiding (UBX-MGA-INI) and saving the receiver's database to
  the card (UBX-MGA-DBD) would help after deep sleep, which loses everything:
  P02 powers the whole receiver.
