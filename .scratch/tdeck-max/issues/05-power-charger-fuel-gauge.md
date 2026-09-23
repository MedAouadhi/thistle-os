# 05: Battery reporting on the T-Deck Max and T-Deck Pro

**What to build:** Accurate battery percentage, voltage and charge state on both boards.

One driver serves both, and serves two different charger parts. The SY6970 and BQ25896 are register-map clones — same register map, same scaling constants, same bit-level decode for battery voltage and charge status — differing only in I²C address, maximum charge current, and a few extra bus-status codes. They are a variant, not two drivers. Which part a T-Deck Max carries depends on its hardware revision, so the driver probes rather than trusting configuration.

This also fixes a live bug on the T-Deck Pro, whose board profile declares a charger the board does not have and reads battery voltage from an ADC pin that is physically the LoRa reset line. The Pro has the same fuel gauge and a register-compatible charger, so the fix is shared rather than Max-specific.

Two initialisation requirements are not optional on this hardware, and both fail silently rather than loudly. The charger's I²C watchdog resets its configuration registers to defaults on expiry, undoing charge current and input limit settings while the system idles. And its ADC does not free-run by default, so the voltage register reads stale until a conversion is triggered. Neither chip's interrupt is routed to the SoC, so polling is the only option.

**Blocked by:** 03 (T-Deck Max boots to a usable system).

**Status:** done

- [x] A single driver registers the power HAL interface and reads both the charger and the fuel gauge
- [x] The driver probes both charger addresses and identifies the variant from the part-number field of the device identification register, not the revision field — the vendor datasheet documents the revision as starting from zero, so a die revision would invalidate it
- [x] Battery percentage comes from the fuel gauge, with a voltage curve only as fallback when the gauge is absent
- [x] Battery voltage and charge state come from the charger
- [x] Charge state distinguishes not charging, charging, and charged
- [x] The charger's I²C watchdog is disabled or fed, so its configuration survives an idle system
- [x] Continuous ADC conversion is enabled, so battery voltage is not read stale
- [x] The fuel gauge is read without unsealing, which its documentation confirms is unnecessary for these reads
- [x] Register decoding is unit-tested as pure functions against datasheet values, covering both variants, the signed handling of fuel gauge current, and the documented behaviour that charge current latches its last value when the charger disconnects
- [x] The T-Deck Max board profile uses this driver
- [x] The T-Deck Pro board profile is switched to this driver and no longer reads battery voltage from its LoRa reset pin

## Comments

The driver is `components/kernel_rs/src/drv_power_sy6970.rs`, board id
`com.thistle.drv.power-sy6970`. Naming a driver after one half of a variant
pair is uncomfortable, but the alternatives are worse: a hyphenated pair name
is long and still arbitrary in its ordering, and a board-derived name is what
the glossary tells us not to do. The SY6970 is the part LilyGo ship today —
their own pinmap marks the BQ25896 "now deprecated" — so it is the one in the
name. The vtable reports `SY6970/BQ25896`, and the bring-up log names whichever
part actually answered.

### The part number does distinguish them, and the revision would not have

Worth recording, because the vendor's own library does the opposite of what
this ticket asks and it would be easy to copy.

- **SY6970** — REG14 `PN[2:0]` = **001**, and `DEV_REV[1:0]` documented as
  "Start from 00" (Silergy SY6970 datasheet Rev. 0.1, REG14 table).
- **BQ25896** — REG14 `PN[2:0]` = **000**, `DEV_REV[1:0]` = 10 (TI BQ25896
  datasheet, REG14).

XPowersLib, which LilyGo vendor in the T-Deck Max repository, identifies the
chip with `readRegister(0x14) & 0x03` — the revision field — and compares it
against `SY6970_DEV_REV 0x00` / `BQ25896_DEV_REV 0x02`. That works today and
stops working the first time either part is spun. The part number is a
different field in the same byte and is the one both datasheets define as the
device's identity, so that is what this driver reads. A test asserts every
revision of both parts still identifies correctly.

The address narrows the search but does not decide the answer: both addresses
are probed, and whichever answers is asked what it is. Identity and address
stay separate facts, and the bring-up log reports the address that actually
answered rather than the one the part number implies, so a disagreement is
visible instead of papered over.

**The failure mode this chooses, deliberately.** A part that answers and is
neither is left closed, so a charger reporting an unexpected part number means
no power HAL interface at all — where XPowersLib would have driven it anyway.
That is the harder failure, and it is the one to want here: an unrecognised
part number means an unrecognised register map, and the next thing this driver
does is write to configuration registers. It is also the stance the rest of
this effort already takes — ticket 04's "a detector that returns unidentified
is more useful than one that returns the wrong board", and ticket 02's expander
that stops the boot rather than letting drivers probe rails that were never
raised. If a shipping unit turns out to report a third part number, the fix is
to add it to `charger_variant`, not to loosen the check.

### Configuration arrives as JSON, not as a config struct

Every other compiled-in driver is handed a `#[repr(C)]` config struct built by
`board_builtin_driver_init` in C. This one is Rust with no C component to carry
a header, so that pattern would mean the same struct declared in two places
with nothing checking they agree.

Instead the C branch is one line — `drv_power_sy6970_register(config_json)` —
and the driver reads its own driver entry. That also sidesteps a trap in the
existing pattern: the registry stores the config *pointer* and `init` runs much
later, from the driver manager, so the existing branches all need `static`
config structs. A JSON string from the board profile path is a temporary, and
handing that to the registry would dangle. Reading it at registration time and
keeping the result is the shape an ELF driver already has
(`driver_init(const char *config_json)`), so this driver can move out to an ELF
without changing how it is configured.

The entry says only which bus the two chips sit on, plus the gauge's address as
an override for a board that wires it elsewhere. The charger's address is
deliberately not configurable: the board cannot know which part its revision
shipped with, which is the whole point of probing.

### A fake bus, so init is not untestable

Seam 2 as specified is the pure decode functions, and those carry the bulk of
the tests — decode arithmetic against the values in the datasheet tables, both
variants' part numbers, the signed gauge current, and the charge-current latch.

But two acceptance criteria — the watchdog disabled and the converter running —
are about the state the driver leaves the charger in, which no pure function
can show. Both chips are plain register files over I²C, which is little enough
to model, so the host build's I²C stubs are a small fake bus: a device answers
at an address or it does not, and what the driver writes stays written. That
makes "the watchdog is off and the rest of the timer register is untouched"
something a test reads back rather than something only hardware can confirm.

It follows the simulator's XL9555 device model in spirit. It is not in the
simulator: the simulator registers its own scripted power driver and stubs
`board_builtin_driver_init` out entirely, so this driver never runs there.

### The currents and the gauge's own voltage have no HAL field

`hal_power_driver_t` carries voltage, percent and state, and nothing else. The
charge-current, gauge-current and gauge-voltage decodes are required by the
ticket and tested, but the only thing that reads them is the line the driver
logs when it comes up:

```
SY6970 at 0x6a: 3824 mV, 42 %, charging at 500 mA, gauge 3780 mV / -100 mA
```

The two voltages are there to be compared: the charger's converter and the
gauge each measure the same cell independently, so a board where they disagree
has something wrong that no single reading would show.

That is deliberate rather than accidental. This ticket cannot be closed without
a board on a desk, and that one line is what the person holding the board will
read first: which part answered, at which address, and whether the numbers are
plausible. Widening the power HAL to carry current is a kernel ABI change and
is not this ticket.

### The T-Deck Pro's compiled-in initialiser, too

`components/board_tdeck_pro/src/board_tdeck_pro.c` registered the TP4065B
driver against ADC1 channel 3 — GPIO 4, the LoRa reset line — exactly the bug
the board profile had. That file only runs when no board profile exists *and*
detection fails, which since ticket 04 is a narrow path, but it is a path that
configures a radio control line as an ADC input, so it is switched over too.

That needed one other thing: the compiled-in board initialisers create their
I²C bus and never register it with the HAL bus registry, so a driver that asks
for a bus by index finds nothing. The Pro's now registers it. Unchanged for the
T-Deck, which has no driver on that path that asks.

Which of `board_tdeck.c` and `board_tdeck_pro.c` actually links is not
established — both define `board_init` strongly, and only the first archive the
linker reaches is pulled in. That is the same shape of ambiguity ticket 09
flags for the IMU, and it is not resolved here.

### On hardware

A T-Deck Max was flashed with this build and booted. What the boot log shows:

- Detection scans SDA 13 / SCL 14, finds `TCA8418=1 CST328=1 BHI260=1
  XL9555=1`, and names the board `LilyGo T-Deck Pro Max`. The XL9555 rule works
  on real silicon.
- The embedded fallback profile it writes carries this driver entry, and it
  arrives intact: `power-sy6970: {"i2c_bus": 0, "gauge_i2c_addr": "0x55"}`.
- `power driver registered: SY6970/BQ25896`.

So the whole path from detection through the board profile to
`hal_power_register` is confirmed on hardware.

**The reading is confirmed.** Reported at the boot-health milestone, which is
the first quiet moment after the burst:

```
I (61048) thistle: Battery: 4284 mV, 100% (SY6970/BQ25896, up on retry)
```

4284 mV on a charged cell, percentage agreeing. So the driver reaches a
charger, configures it, reads its voltage register and reports a plausible
value on real hardware.

**"up on retry" is the interesting part, and it is a real finding.** The driver
does *not* come up when the driver manager initialises it during the boot
burst — `get_info` returned ESP_ERR_INVALID_STATE, meaning `variant` was still
`None` — and it does come up a few seconds later, unchanged. Something the
charger needs is not ready at ~2.2 s but is by ~61 s. Worth understanding
rather than leaving to a retry: the obvious suspect is a rail behind the IO
expander still settling, which would affect other drivers on this board too.

Two things remain unknown:

- **Which of the two parts answered.** The vtable names the pair, and the
  driver's own line naming the part and address still does not appear, even at
  the quiet retry — so the `power_logi!` macro emits nothing on device although
  `hal_registry`'s near-identical one does. A loose end in this driver. LilyGo
  document this board as shipping the SY6970 at 0x6A and the probe tries that
  address first, so it is very likely the SY6970 — but that is inference, not
  evidence.
- **Whether the percentage came from the gauge or the curve.** At 4284 mV both
  give 100, so this reading cannot tell them apart.

**Getting there needed a fix in another ticket's code.** The keyboard driver's
LEDC speed-mode constant was the ESP32's value, which is out of range on an
ESP32-S3, so its init returned ESP_ERR_INVALID_ARG. An input driver failing is
fatal to `hal_registry_start_all`, so the sequence stopped before power was
ever initialised — `driver start failed: 258`, and no power init at all. Fixed
separately against ticket 03; the board now boots past it.

**A build trap worth knowing.** `idf.py build` can report success while linking
against a previous run's Rust objects: the POST_BUILD step that merges the Rust
staticlib into the component archive only runs when CMake relinks that
component, and the touch meant to force it happens too late in an incremental
build. A firmware was flashed and drew a wrong conclusion before this was
spotted. `touch components/kernel_rs/stub.c` before building forces the merge;
the real fix is to make the merge depend on the Rust library file.

### What hardware still has to confirm

Everything below is decoded from datasheet tables and asserted against them,
not against a board:

- that the fuel gauge is populated and answers sealed on both boards
- whether the reported percentage tracks a real discharge
- whether the SY6970's `DEV_REV` really is the only field the vendor's own
  library could have keyed off — if some shipping unit reports an unexpected
  part number, this driver declines it where XPowersLib would have accepted it

The Pro's parts were checked rather than assumed: LilyGo document the T-Deck
Pro as carrying a BQ25896 at 0x6B and a BQ27220 at 0x55, on SDA 13 / SCL 14,
which is the bus its board profile declares as index 0. So the Pro is a
register-compatible charger and the same gauge, as the ticket says, and the
entry needs no board-specific configuration.

### From review

A standards and spec review ran over the change. Acted on:

- **The prototype was hand-copied into two translation units.** Now a real
  header, `components/kernel_rs/include/drv_power_sy6970.h`, which both include
  — so the ABI is declared once and the compiler checks it.
- **`board_tdeck_pro` called into kernel_rs without requiring it.** Added to
  its `REQUIRES`; an undeclared cross-component dependency leaves link order to
  luck.
- **Both new calls in `board_tdeck_pro.c` discarded their `esp_err_t`.** The
  bus registration is now fatal like the bus init above it, and a failed power
  registration logs a warning — non-fatal, matching how the HAL registry
  already treats a power driver.
- **`cargo test --features sim-bus` did not compile.** The bring-up tests need
  the fake bus, which that feature replaces with the simulator's virtual one.
  They are gated on it now, and that configuration builds.
- **The discharge curve was borrowed out of the TP4065B driver.** It describes
  the cell, not either charger, and a driver reaching into a sibling driver
  does not survive the move to standalone ELFs. Moved to `battery_curve.rs`
  with its tests; the TP4065B driver now uses it too.
- **`drv_power_sy6970_get()` had no caller** — this driver registers itself, so
  the accessor every other driver exports was dead exported API. Removed.
- **The log named the address the part number implies, not the one probed.**
  Fixed, with a test that the two are separate facts.
- **`gauge_voltage_mv` was decoded and tested but never used.** Now the
  cross-check in the bring-up log.

Not acted on, deliberately:

- **Quoted-hex address parsing exists three times** (C `json_int`,
  `board_config.rs`'s `json_get_address`, this driver's `parse_address`).
  Consolidating means either a driver depending on `board_config` — backwards —
  or moving the helper into `manifest.rs` and re-testing a parser two other
  boot paths depend on. Left as a small, noted duplication.
- **Variant identity affects only the log.** The parts' differing maximum
  charge current and extra bus-status codes are real, but this driver sets no
  charge current and the power HAL has nowhere to put a bus status. Encoding
  either now would be shape without a caller.
- **The compiled-in initialiser change reads as unasked.** Kept: it is the same
  defect in the same driver's area, and what it replaces configures a radio
  control line as an ADC input.

### Checks run

- kernel test suite: 1448 pass (47 of them this driver's)
- `cargo check --features sim-bus --all-targets`, the configuration the
  simulator builds
- `tools/validate_board_catalog.py`, including the embedded fallback profiles,
  which both boards' profiles had to be updated alongside
- `tools/test_validate_board_catalog.py`, `tools/test_target_matrix.py`
- simulator unit tests: 36 pass
- the board bus init host test, built with `-Werror`
- `idf.py build` for esp32s3, clean

## Both open questions are now answered, and one earlier finding was wrong

Once the keyboard's LEDC struct layouts were corrected (ticket 03), the whole
start sequence ran clean and the driver's own line finally appeared:

```
Starting power...power: SY6970 at 0x6a: 4224 mV, 100 %, charging at 250 mA, gauge 4205 mV / -36 mA
I (7659) thistle: Battery: 4224 mV, 100%, charged (SY6970/BQ25896)
```

- **Which part answered: the SY6970, at 0x6A.** The part-number field of the
  device identification register decoded to `0b001`, matching LilyGo's
  documentation for this board. Inference is now evidence, and the decision to
  read the part-number field rather than the revision field holds up on real
  silicon.
- **Gauge or curve.** The gauge is present and answering independently — it
  reports 4205 mV against the charger's 4224 mV, and a current of -36 mA, whose
  sign is the discharge direction the signed decode was written for. Both
  paths are live; the percentage still cannot be attributed from a charged
  cell, but the gauge is no longer hypothetical.
- **The `power_logi!` macro was never broken.** The line was being emitted all
  along and dropped by the console during the boot burst — the same USB-
  Serial-JTAG line loss that hid the `ledc` error. Nothing to fix here.

**And the "up on retry" finding was a misreading.** The claim above — that
something the charger needs is not ready at ~2.2 s but is by ~61 s, with a
settling rail behind the IO expander as the suspect — is wrong. There is no
settling problem. Power init had failed outright, for exactly the same reason
the keyboard had: `hal_registry_start_all()` stopped at the first failing input
driver and never reached power at all. With that fixed, power initialises in
the boot sequence at ~2.3 s and the retry never fires. The retry in `main` is
now dead code on this board; it is kept because a charger that genuinely is not
ready at boot is a plausible thing for another board to do, and the retry costs
one call.


## Closed on hardware

Every acceptance item is verified on a real T-Deck Max, with the board's own
console, on a port nobody else was holding:

```
power: SY6970 at 0x6a: 4184 mV, 98 %, charging at 250 mA, gauge 4169 mV / -34 mA
Battery: 4184 mV, 98%, charged (SY6970/BQ25896)
ThistleOS ready
```

- The variant decode is right on real silicon: the part-number field reads
  `0b001`, the SY6970, at 0x6A — the address and part LilyGo document for this
  board. The decision to read the part number rather than the revision holds up.
- Charger and gauge are both live and answering independently: 4184 mV from the
  charger against 4169 mV from the gauge, and a gauge current of -34 mA whose
  sign is the discharge direction the signed decode was written for.
- The driver initialises inside the normal boot sequence at ~2.3 s. The retry
  in `main.c` never fires on this board; it is kept because a charger that
  genuinely is not ready at boot is a plausible thing for another board to do,
  and it costs one call.
- No `Driver start: ... failed` line at the boot-health milestone, so every
  driver on the board starts clean alongside it.

The T-Deck Pro half is not hardware-verified — no Pro was on the bench. It
shares the driver, the decode and the tests, and its board profile no longer
reads battery voltage from the LoRa reset pin, but someone should put it on a
Pro before trusting it there.
