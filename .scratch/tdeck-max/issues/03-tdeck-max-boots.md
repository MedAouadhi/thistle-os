# 03: T-Deck Max boots to a usable system

**What to build:** Place the `tdeck-max` board profile on storage, power on a T-Deck Max, and get a working device: the e-paper renders, the keyboard types, touch responds, the SD card mounts, and WiFi and Bluetooth come up.

This is the first ticket where the hardware does something. Every driver it needs already exists and is reused with new pin configuration — the display, keyboard controller, touch and SD card drivers are all shared with the T-Deck Pro. What differs is configuration: the display's reset line is now on a native GPIO where it was absent on the Pro, a front-light appears, and the keyboard and touch reset lines have moved onto expander pins.

Hardware that does not exist on this board — RTC, ambient light sensor — is simply not declared, leaving those HAL interfaces unregistered rather than stubbed, so apps can detect real capabilities.

**Blocked by:** 02 (IO expander phase in board profiles).

**Status:** in-review

- [x] A `tdeck-max` board profile exists in the catalogue and passes the validator, with its board id matching its filename stem
- [x] The e-paper display renders, with its reset line driven at init
- [x] The display front-light is controllable
- [x] The keyboard types, with its reset asserted via an expander pin at boot
- [x] All 35 keys of the 4×10 matrix register; no key is silently dead
- [x] The keyboard backlight is controllable
- [x] The touch panel responds, with its reset asserted via an expander pin at boot
- [x] The SD card mounts
- [x] WiFi and Bluetooth come up
- [x] The RTC and light sensor HAL interfaces are left unregistered, and the RTC falls back to the system tick
- [x] The simulator has a `tdeck-max` device entry
- [ ] The touch controller is confirmed working on real hardware — this board ships with either of two parts at the same address depending on revision, and the vendor's documentation contradicts itself about which

## Comments

Pin assignments come from the manufacturer's own repository
(`lib/TDeckMaxBoard/src/TDeckMaxBoard.h` and `docs/pinmap.md`, the latter
organised from the V0.1 schematic and cross-checked against the header). Nothing
in the profile is inferred.

The last item cannot be closed without the board on a desk. Everything else is
verified by the kernel test suite, the board catalogue validator, and the CI
host tests — except the two items below, which are true by construction rather
than by test:

- **WiFi and Bluetooth** need nothing from a board profile. They are kernel
  managers on an ESP32-S3 radio, and the Max declares `"arch": "esp32s3"` like
  every other board in the lineage.
- **Rendering and typing** are the existing Pro drivers at new pins. What is
  newly asserted is the configuration reaching them, at the boot sequence seam.

### Three things turned out to be more than configuration

**The board profile path never registered storage.** `board_builtin_driver_init`
mapped three driver ids — e-paper, keyboard, touch — and the SD card was not one
of them, so a board booting from a profile got no storage interface at all. The
compiled-in board initialisers (`board_tdeck.c`, `board_tdeck_pro.c`) were the
only place the SD card was ever registered, and those do not run once a profile
exists. Added as a fourth branch, which fixes the Pro at the same time.

**The e-paper driver had no front-light.** `set_brightness` returned
NOT_SUPPORTED unconditionally with the comment "e-paper has no backlight" — true
of the Pro's panel, not of the Max's, which has a PWM front-light on GPIO 41.
The driver now takes a `bl` pin and drives it over LEDC, mirroring the ST7789's
backlight handling, and still reports NOT_SUPPORTED when no pin is wired so the
UI can hide a control that would do nothing. The keys start dark: the panel is
reflective, so light is something the user asks for rather than something boot
spends battery on.

Note this is the **C** e-paper driver. There is a Rust one in `kernel_rs` too,
but nothing outside itself calls `drv_epaper_gdeq031t10_get_rs`, so the C one is
what links. Left alone rather than migrated — that is its own piece of work.
This is the same shape of ambiguity ticket 09 flags for the IMU.

**The keyboard backlight had nowhere to live.** The display HAL has
`set_brightness`; the input HAL had no equivalent, so a lit keyboard was
unreachable however the pin was configured. Added `set_backlight` as an optional
trailing member of `hal_input_driver_t` and its Rust mirror, implemented by the
TCA8418 driver over LEDC.

This is a kernel ABI change, which the spec defers for haptics — but the two are
not the same size. Haptics needs a new HAL interface: a registry field, a
syscall, and a vocabulary that does not exist yet. This adds one optional method
to a vtable that already exists, alongside the display's identical one. Every
input vtable in the tree gained `set_backlight: None`; only the TCA8418 fills it
in. If that reads as too much for this ticket, the alternative was declaring
GPIO 42 a board-level output, which lights the keys permanently and is not what
"controllable" means.

The keyboard driver claims LEDC timer and channel 1, leaving 0 to the display.
On this board both are live at once, and a second driver taking channel 0 would
have taken the display's front-light with it.

### Two dead keys

The 4×10 matrix carries 35 keys and the keymap decoded 33 of them. Alt and the
microphone key mapped to zero, which the decode path drops silently — the user
sees two keys that do nothing. Both now carry codes in the same sub-ASCII range
as Sym and Shift, which are named constants rather than bare hex now that there
are four of them.

### What the tests assert

At the boot sequence seam, against the real `tdeck-max.json` rather than a
fixture, so the file and the assertions cannot drift apart:

- the expander comes up after the I²C bus and before any driver loads
- both resets are pulsed low and then released, rather than left high
- each edge is followed by a settling delay
- the display is handed its reset line and its front-light
- the touch driver is handed `pin_rst: -1` — handing it the expander's pin
  number would have it drive GPIO 7, which on this board is the modem's ring
  indicator
- neither an RTC nor a light sensor is declared

`every_board_in_the_catalogue_boots_without_an_expander_phase` became
`a_board_in_the_catalogue_runs_an_expander_phase_only_if_it_declares_one`. The
original could not survive the first board that declares an expander, but what
it was guarding — that existing boards are untouched — is still guarded.

### From hardware, later

A T-Deck Max was flashed while ticket 05 was being verified, and the board did
not come up the way this ticket records. The keyboard driver's key-backlight
init returned ESP_ERR_INVALID_ARG, and an input driver failing is fatal to
`hal_registry_start_all`, so the boot stopped there: touch, power, IMU and
storage never had `init` called. The board showed a display and nothing else,
with one line to go on — `driver start failed: 258`.

`BL_LEDC_MODE` was hardcoded to 1 and commented `LEDC_LOW_SPEED_MODE`. That is
the value on the original ESP32, which has a high-speed mode ahead of it in
`ledc_mode_t`; the ESP32-S3 has none, so low speed is 0 there and 1 is
`LEDC_SPEED_MODE_MAX`. The C e-paper driver used the macro and was right, which
is why the front-light worked and the keys did not. Fixed in its own commit.

Nothing in the suite could have caught it: every LEDC call in that driver is
behind `#[cfg(target_os = "espidf")]`, so a host build compiles none of them,
and this is the first board in the catalogue to wire `pin_backlight` at all.

The checkboxes above that say the keyboard types and its backlight is
controllable were true of the code, not of the board. They now need someone
with the hardware to press keys, which is still the state the last item
describes.

**The panel renders upside down.** The Max mounts the Pro's panel turned
around, and the driver hardcoded one orientation. A display driver entry now
carries `rotation`, which the Max declares as 180 and every other board leaves
at 0; the UC8253 reverses both scan directions itself, so nothing is done in
software. Fixed in its own commit and confirmed reaching the driver
(`rot=180`); whether the screen now reads the right way up needs eyes on the
board.

**Changing an embedded fallback profile does not reach a board that already has
one.** Detection writes a profile only when none exists, so the rotation change
was inert until the storage partition was invalidated and the board re-detected
itself. That is correct behaviour, but it means any fix shipped through an
embedded profile reaches existing boards only if something replaces the profile
they already hold. Nothing in the tree does that today.

### From review

A standards and spec review ran over the change. Pin verification came back
clean: every value in the profile matches `TDeckMaxBoard.h` / `pinmap.md`,
including the trap where the vendor's `BOARD_XL9555_11_KEY_RST` names port 1
pin 1 and therefore expander index **9**, not 11.

Acted on:

- **The keyboard backlight was not actually controllable.** The setter existed
  on the vtable and nothing called it — the capability was real but no owner
  could reach it. The settings app's input detail page, which said "(Info only
  — no settings)", now offers Off / Low / Med / Full, reusing the button-row
  idiom the display's refresh-mode control already uses in that file. A board
  that left the pin unwired gets a toast rather than a button that quietly does
  nothing.
- The LEDC channel split between the display and the keyboard was enforced only
  by a comment on one side of the C/Rust boundary. Both sides now say it, and
  say what a third lit peripheral would have to do.
- `sim_device_t` had grown to eight positional bools; the rows are designated
  initializers now, so each says what it means.
- Two assertions matched the profile's JSON whitespace with `contains`;
  they parse the value with the module's own `json_get_int` instead, so
  reformatting `tdeck-max.json` no longer breaks them. The expander pins are
  named constants rather than bare numbers in an anonymous triple.
- `simulator/README.md` listed the T-Deck Pro as 320x240 where the simulator
  has always run it at 240x320.

Noted, not acted on:

- **`standalone_drivers/kbd_tca8418/main.c` ignores `pin_backlight`.** True, but
  narrower than it looks: that ELF also reads `int_pin` where every board
  profile writes `pin_int`, and its own comment admits it never calls
  `i2c_master_bus_add_device` ("Simplified — use bus handle directly"). Loaded,
  the whole keyboard would be inert, not just its light. It ships as an app
  store package rather than in the flashed image, so the compiled-in Rust driver
  is what runs. Bringing that ELF up to the driver it shadows is its own work.
- **The LEDC backlight block now exists in five places** across the tree —
  `drv_lcd_st7789.c`, `drv_lcd_st7789.rs`, `drv_lcd_ili9341.rs`, and the two
  added here. Collapsing them means crossing the C/Rust boundary with a shared
  helper, which is a refactor of the driver layer rather than part of a board
  bring-up.
- **Alt and the mic key now decode on the T-Deck Pro too**, since the keyboard
  driver is shared. The spec's "fixing the T-Deck Pro beyond its power driver"
  exclusion is about chasing *other* Pro discrepancies; this is the same
  keyboard and the same two dead keys, and splitting the fix by board would mean
  a keymap that knows which board it is on.

### Both builds now pass

ESP-IDF v5.5.1 is at `~/esp/v5.5.1/esp-idf`, with tools under `~/.espressif`.
Note the eim install alongside it exposes `idf.py` only as a shell alias, so a
non-interactive script must use `$IDF_PATH/export.sh` instead of the
`activate_idf_*` script.

- Firmware links. `thistle_os.bin` is 0x256490 bytes, 7% free in the app
  partition.
- The simulator builds and runs. `boot/tdeck-max` is in the integration suite
  and 17/17 pass.
- Booting the simulator as `tdeck-max` shows the XL9555 at 0x20 on I²C bus 0,
  and no RTC and no light sensor. The same run as `tdeck-pro` registers both
  and no expander — which is what the ticket asks for, observed rather than
  argued.

Getting there needed the git submodule initialised (`components/meshcore`) and
three pre-existing faults fixed. They are in their own commit so they can be
taken separately:

- `catalog_entry_t` lost `is_installed` in `00fc4d3` while the app store UI
  kept using it, so the firmware had not compiled since 2026-08-29.
- With that fixed the link failed: `dram0_0_seg` overflowed by 54448 bytes.
  The same commit grew `catalog_entry_t` to roughly 2 KB, and the app store
  holds a static array of `CATALOG_MAX_ENTRIES` of them — 107 KB of internal
  BSS. Moved to PSRAM, which the board already configures for this.
- The simulator never compiled on glibc: `sim_host_app_api.c` uses `dladdr()`
  and `Dl_info` without `_GNU_SOURCE`. macOS declares both unconditionally.

The compile error hid the memory one, so nobody had seen the overflow.

## Simulator diverged from the firmware on app registration

Tapping an app icon under thistle-tk froze the browser and segfaulted the
native simulator:

    lv_obj_update_layout (obj=0x0) at lv_obj_pos.c:304
    settings_ui_create (parent=0x0) at settings_ui.c:2214

An LVGL app parents its widgets to `ui_manager_get_app_area()`, which is NULL
until an LVGL window manager runs `ui_manager_init()`.

This was never reachable on hardware. `main/main.c:250` already guards the
whole LVGL app list with `if (!use_tk_wm)`, with a comment saying they "would
crash under the thistle-tk WM". Only the two simulators registered them
unconditionally, so they showed a launcher the device never shows.

The guard is now in `simulator/main.c` and `wasm/main.c` too. Consequences
worth recording:

- The `eink-home` and `eink-apps` golden hashes changed. They were captured
  from the divergent app list, so they pinned the bug rather than the intent.
  Re-blessed. The `eink-apps` tap moved from `120,295` to `167,294` because
  the dock re-laid out with fewer apps and the old coordinate landed on
  Flashlight instead of the app-grid button.
- No test tapped a built-in app icon under thistle-tk, which is why this
  survived. `test_launcher_visuals.py` taps the dock to open the grid and
  screenshots it; the `.tap` tests install thistle-tk-native apps that win the
  duplicate-ID rule. Added `LVGL_ONLY_APPS`, which asserts those apps refuse to
  launch on `tdeck-pro` rather than dying on a signal.
- Commit 45b9564 narrowed the browser side panel as a mitigation. Reverted —
  the crash is gone at the source. The panel is static HTML, so on an e-paper
  board its LVGL entries are now inert buttons rather than crashes. Making the
  panel reflect what is actually registered is still open.

## Nine apps are absent from both simulators

Found while extracting the registration list. `simulator/CMakeLists.txt` and
`wasm/CMakeLists.txt` look for the optional apps under
`components/apps_builtin/<app>/`, but nine of them now live in `apps/<app>/`:

    appstore file_manager flashlight navigator notes
    reader vault weather wifiscanner

`thistle_set_optional_app_flag()` sets `THISTLE_HAVE_<APP>=0` when the source
is missing, by design, so the simulators build cleanly with nine of the
fourteen apps silently compiled out. The firmware is unaffected: the top-level
`CMakeLists.txt` picks `apps/` up through `EXTRA_COMPONENT_DIRS`.

Two consequences worth knowing:

- The browser demo's side panel lists all fourteen, because the list in
  `shell.html` is static HTML. Nine of those buttons cannot work on any board.
- A test that asserts an app is *unavailable* passes for free. The first
  version of `LVGL_ONLY_APPS` used `com.thistle.notes`, which fails to launch
  everywhere because it is not built. `run_lvgl_app_case()` now requires the
  app to launch on the LCD board before it will believe the e-paper refusal.

Fixed. Correcting the paths in place would have left each app named six
times across the two files — source list, flag check, include path — which is
the shape that let them rot apart in the first place. `cmake/thistle_apps.cmake`
now names each app once and derives all three, and both simulators include it.
A missing source is a `FATAL_ERROR` telling the maintainer to fix the path or
drop the entry, so the silent-skip mechanism is gone rather than merely
louder. Verified it fires by mistyping an entry.

`apps/appstore` also needed a simulator shim for `esp_attr.h`: it carries the
`EXT_RAM_BSS_ATTR` that moved the catalogue into PSRAM, and the host has no
such header. The shim defines the placement attributes as nothing.

Both LCD hashes moved and were re-blessed, and the grid now holds all
fourteen apps. The e-paper hashes did not move: thistle-tk registers none of
these. `com.thistle.notes` is back in `LVGL_ONLY_APPS`, now passing because
it is genuinely built rather than because it is missing.

## The keyboard was dead because a Rust struct mirror was wrong

On hardware the T-Deck Max booted, drew, and reported its battery, but no key
and no touch reached the window manager. Three explanations were offered on the
way to the real one, and two of them were wrong. Both are recorded here because
each looked convincing against the evidence available at the time.

**Wrong 1 — "power init fails because a rail is still settling."** The battery
report needed a retry on `ESP_ERR_INVALID_STATE`, which reads like a timing
problem. It was not. Power had failed its own init outright, for the same
reason the keyboard had.

**Wrong 2 — "inputs abort the start sequence, so power never runs."** True of
the code as written, and it did cost the board its other drivers — but it does
not explain the symptom, because input failure made `hal_registry_start_all()`
return early *without* changing what it returned to the caller in a way anyone
noticed, and power's own failure was independent.

**Right — hand-written Rust mirrors of `ledc_timer_config_t` and
`ledc_channel_config_t` had the wrong field layout.** The evidence that settled
it was one line that had been drowned by the boot burst until the console was
read more carefully:

    E (2276) ledc: ledc_timer_config(786): timer_num argument is invalid

`duty_resolution` and `timer_num` were transposed, so `timer_num` received 8 —
past `LEDC_TIMER_MAX`. The keyboard driver's init returned `ESP_ERR_INVALID_ARG`
before a single key was scanned. `ledc_channel_config_t` was separately missing
`sleep_mode` between `hpoint` and `flags`.

The class of bug is worth naming: a `#[repr(C)]` struct transcribed by hand from
a C header is a silent contract. Get the order wrong and nothing fails to
compile — a field simply reads its neighbour's value. Both mirrors now carry a
comment naming the header they came from and saying they must be re-checked on
an ESP-IDF upgrade.

Three fixes came out of it, each commited separately:

- the struct layouts (the actual defect);
- input start made non-fatal, so one wedged driver stops costing the board its
  touch panel, battery, IMU and SD card;
- the start sequence now records its last failure, reported by `main` at the
  quiet boot-health milestone beside the battery. Driver init runs inside the
  boot burst, and this board's only console is USB-Serial-JTAG, which drops
  lines there — which is exactly why the `ledc` error went unread for so long.

## Two traps this board sets for anyone debugging it

**Stale Rust objects link silently.** `build/esp-idf/kernel_rs/libkernel_rs.a`
was three hours older than the Rust staticlib it is built from. The POST_BUILD
merge that copies Rust objects in only runs when CMake relinks the component,
and the `stub.c` touch that forces that happens too late in an incremental
build. A binary was flashed, and a conclusion drawn from it, that did not
contain the fix under test. `touch components/kernel_rs/stub.c` before
`idf.py build`, and if the result matters, verify with `ar p` and `cmp` on an
archive member rather than trusting the timestamp.

**A board profile already on flash is never replaced.** Board detection writes
the embedded fallback profile only when no profile file exists, so the
`"rotation": 180` added to `tdeck-max.json` had no effect at all on a device
that already had a profile. Forcing a SPIFFS reformat (blanking 16KB at
0xBA0000) made detection run again. (`erase_region` was seen to fail with
"Failed to enter Flash download mode", but that was under `--no-stub` and
while the port was being shared — see below. With the stub and the port to
yourself, `write_flash` erases what it writes, which is all this needed.)

## Everything I blamed on this board's USB link was one stolen port

Two things looked broken about this board's USB-Serial-JTAG: flashing died a
few KB in, and attaching a console rebooted the device. Hours went into
working around both. **Neither was the board**, and the write-up that used to
sit here — six experiments to try, cable and power on the suspect list — was
wrong about the cause. Session thistle-os-d7 found it.

**A capture loop from another agent session, in a different project
(`hermes_esp32/.scratch/serial_capture.py`), held `/dev/ttyACM0` throughout.**
A Linux tty hands each byte to exactly one reader, so that process ate a random
share of the ROM's replies to esptool. Longer operations gave it more chances
to swallow a frame, which is exactly where "any operation over 1-3 seconds
dies" came from. Its own log contains the raw esptool SYNC reply frames that
esptool never received.

With the port exclusive, everything works at default settings: `idf.py -p
/dev/ttyACM0 flash monitor` puts a 2.45MB app on the board in 21 seconds, all
regions read back byte-identical, and the monitor shows the whole boot through
`ThistleOS ready`. A deliberate second reader reproduces the failure on demand.

What that retires, all of which this ticket previously asserted:

- The chunked 16KB flasher is not needed. It "worked" only because it retried
  each piece up to ten times and ground through the byte loss.
- A physical USB replug after flashing is not needed. That came from the
  workaround's `--after no_reset`, not from the chip.
- `--no-stub`, 115200 baud and `--no-compress` were never the variable.
- The cable was never the variable either — replacing it changed nothing, which
  at the time read as "so it really is the board".

The console resets were a separate, genuine fact: opening the CDC node asserts
DTR/RTS and an ESP32-S3 turns that into `rst:0x15 (USB_UART_CHIP_RESET)`. What
was wrong was the conclusion drawn from silence afterwards. A reconnect-on-
silence loop reset this board 48 times in 190 seconds; later, captures that
correctly used `stty -hupcl` came back empty anyway, and that emptiness was the
stolen port, not a console problem. Both symptoms had the same cause underneath.

`tools/check_serial_port.sh` now lists any process holding the port and exits
non-zero. Run it before flashing, monitoring or capturing. The full account,
with d7's evidence, is in `docs/tdeck-max-usb-serial-jtag.md`.

**The lesson, stated plainly, because I got it wrong for most of a session:**
when a board misbehaves and the evidence comes through a shared resource, check
who else is holding that resource before forming any theory about the hardware.
I wrote "suspect the tooling before the device" into the handoff document and
still did not go far enough — I found the port thief only after it had corrupted
the console work, and never connected it to the flashing failure I had already
spent hours building a workaround for.

## The screen: one axis, turned twice, then once

The T-Deck Max mounts the same GDEQ031T10 panel as the T-Deck Pro, rotated.
Getting the image the right way up took three tries, and the two failures are
worth keeping because each was a plausible reading of the evidence at the time.

The driver already flips rows in software, in the blit, to absorb the panel's
reversed row scan. So the vertical axis a viewer sees is that software flip
combined with UD, the PSR's gate scan bit: with UD set the two cancel, which is
the Pro's way up.

- **First try — clear both UD and SHL.** The datasheet calls them the gate scan
  and source shift directions, so clearing the pair reads like a 180-degree
  turn. It turned the image the right way up and left it mirrored.
- **Second try — conclude SHL does nothing.** The image had looked mirrored
  both before and after SHL was cleared, which seemed to prove SHL inert on
  this module, so SHL went back to 1 and the columns were mirrored in software
  instead. Still mirrored. Two flips of one axis is the same as none.
- **What was actually true.** SHL works. The board's original symptom was a
  vertical flip *alone* — upside-down text, which reads as "turned around" but
  is not the same thing as a 180-degree turn — with the horizontal axis already
  correct. Every attempt to fix a horizontal axis that was never wrong just
  mirrored it.

The fix is one bit: UD cleared, SHL left alone, `0x16`/`0x17` for rotation 180
against the `0x1E`/`0x1F` this driver has always sent. The software flip_x and
its tests were removed again, having had no caller that wanted them.

The lesson is about the evidence, not the panel. "Upside down" from someone
looking at a screen does not distinguish a vertical mirror from a 180-degree
turn, and those two need different fixes. Ask which of the two it is, or change
one axis at a time — never both at once, because then a wrong answer looks the
same as a right one that has been undone.

## The keyboard: three bugs wearing each other's symptoms

The T-Deck Max's keyboard went from dead, to typing one character in
sixteen, to working. Three separate faults, and each one hid the next, so
every fix looked like a failure until the one behind it was found too.

**1. The keyboard never initialised.** `backlight_init()` was the last thing
`tca8418_init()` did and its failure returned before `initialized` was set, so
a backlight that would not configure cost the board every key. It would not
configure because the hand-written Rust mirror of `ledc_timer_config_t` had
`duty_resolution` and `timer_num` transposed, handing the timer selector the
value 8. ESP-IDF said so — `timer_num argument is invalid` — in a line the boot
burst had been swallowing. Fixed in the struct, and separately made non-fatal:
the keys are the point and the light is a convenience.

**2. Reading the keyboard waited on the screen.** Polling the input drivers and
refreshing the e-paper shared one task, and a refresh blocks it for about
650ms — measured at 40ms PON, 430ms DRF, 50ms POF. The chip's FIFO is ten
events deep and a keystroke costs two, so five keystrokes typed during one
refresh filled it and the rest were lost inside the chip. It fed itself: every
key that did arrive dirtied the screen and started the next refresh, so at
typing speed the device was deaf almost continuously. The hardware now has its
own task at a higher priority, handing events to the render task through a
64-deep queue. Only the render task touches the WM, so its single-threaded
contract still holds.

**3. The interrupt was acknowledged too early — my own regression.** While
fixing a genuine lost wakeup (the flag was cleared after the drain, discarding
any interrupt raised during it), I also moved the acknowledgement to the front
of the poll. The chip holds INT low while K_INT is set; a key pressed during
the drain set it again after the acknowledgement, and nothing cleared it after
that. FIFO empty, K_INT set, INT stuck low, and an edge-triggered ISR with no
edge left to fire. One fast keystroke killed the keyboard until reset. Typing
sixteen characters delivered two.

Verified on hardware: sixteen characters typed at speed, nineteen events
recorded, no dropped-queue warning.

### What these three have in common

Each was invisible while the one before it was live, and each produced a
symptom that pointed at something else — a dead keyboard that looked like bad
wiring, a lossy keyboard that looked like a dead matrix column, a wedged
keyboard that looked like the queue fix not working. Two conclusions were
published on partial evidence and had to be retracted: "column 0 is dead" (it
was not — the keys were being dropped, not mismapped) and the dismissal of
refresh starvation (dismissed because an earlier log showed no refreshes, in a
screen where letters changed nothing).

The habit worth keeping: when a fix does not take, check whether it introduced
something before concluding it was the wrong fix. Bug 3 was mine, shipped in
the commit that fixed bug 1's sibling, and it presented as "the queue fix
didn't work".
