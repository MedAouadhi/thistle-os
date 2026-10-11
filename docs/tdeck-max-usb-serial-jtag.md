# T-Deck Max: flashing and console over USB-Serial-JTAG

A handoff document. Everything here was established on real hardware between
2026-09-14 and 2026-09-15, on a LilyGo T-Deck Max (ESP32-S3, QFN56, rev v0.2,
MAC `80:45:6b:46:cf:20`, 16MB Winbond flash, 8MB PSRAM), host Linux with
ESP-IDF v5.5.1 and esptool v4.12.dev2.

The board has no UART header in use: its only console and its only flashing
path are the ESP32-S3's built-in USB-Serial-JTAG peripheral, on
`/dev/ttyACM0`. Both looked unreliable, in two separate ways, and each cost
a debugging session. **Both are now solved, and neither was the board.** The
flashing failure was a second process holding the tty (§1). The console
resets were our own tooling (§2). This document stays because the symptoms
look exactly like a hardware fault and will be misread again.

The one-line rule: **before flashing, monitoring, or capturing, run
`tools/check_serial_port.sh /dev/ttyACM0` and stop whatever it lists.**

---

## 1. Solved: flashing died because another process held the port

`esptool write_flash` failed a few KB into a normal flash of this board with

```
A fatal error occurred: The chip stopped responding.
```

(a `StopIteration` out of `esptool/loader.py:421` in `slip_reader`, i.e. the
SLIP stream went silent). A chunked 16KB workaround with retries took six
minutes per image and was the only way to flash. The failure was attributed
to the USB-Serial-JTAG link, and cable, flash health, silicon errata, power and
host USB were all on the suspect list.

### Root cause

A capture loop from **another agent session, in a different project**
(`hermes_esp32/.scratch/serial_capture.py`), had `/dev/ttyACM0` open the
whole time. It believed it was reading its own board; the only USB serial
device on this host was the T-Deck Max. A Linux tty hands each byte to exactly
one reader, so that process consumed a random share of the ROM's replies to
esptool. Longer operations gave it more chances to eat a frame, which is where
"any operation over 1–3 seconds dies" came from.

Evidence, all from 2026-09-15:

- `ls -l /proc/*/fd | grep ttyACM0` showed exactly one holder: the capture
  loop, parented to a `claude` process whose cwd was `hermes_esp32`.
- Its 13MB log contained 575 `thistle:` lines, 760 ROM `waiting for download`
  banners, and, immediately after those banners, raw esptool SYNC reply
  frames: `c0 01 08 04 00 07 07 12 20 00 00 00 00 c0`, repeated. Those bytes
  were the replies esptool never received.
- With that process stopped and no other holder, everything worked at default
  settings on the first try:

  | operation | result |
  |---|---|
  | `read_flash 0x20000 0x200000` (stub, 460800) | 2MB in 47.9 s |
  | `write_flash` bootloader + table + otadata + 2.45MB app (stub, compressed, 460800) | 26.6 s total, app in 21.2 s |
  | read-back of all four regions vs. the images | byte-identical |
  | `idf.py -p /dev/ttyACM0 flash monitor` | flashes, resets, shows the boot |

- Positive control: starting a reader of the same shape on purpose (pyserial,
  DTR/RTS low, 1 s timeout, 4KB reads) made esptool fail at `Connecting...`
  twice in a row with pyserial's own diagnosis, *"device reports readiness to
  read but returned no data (device disconnected or multiple access on
  port?)"*. The reader had captured 26 SLIP frames. Killing it and repeating
  the same 2MB read succeeded again.

### What this retires

- The chunked flasher in `../T-Deck-MAX/tools/flash-usb.sh` is not needed. It
  "worked" because it retried each 16KB piece up to ten times and reconnected
  constantly, so it ground through the byte loss. Its "roughly two operations
  per connection" was the loss rate, not a property of the chip.
- `--no-stub`, 115200 baud, `--no-compress` and small chunks were never the
  variable. The stub, compression and the default baud are fine.
- The board does **not** need a physical USB replug after flashing. That came
  from the workaround's `--after no_reset`. `--after hard_reset` boots the
  app: bootloader output follows it, and a subsequent `--before no_reset`
  connect gets a pyserial *Write timeout* because the app, not the ROM, owns
  the USB peripheral.
- The USB link itself never re-enumerated during any of today's flashing,
  reading or resetting: `journalctl -k` shows the device number unchanged
  across all of it. The link is not flaky.
- Cable and flash health were already ruled out; silicon revision, power and
  host USB never needed testing.

Not retested: `erase_region` with `--no-stub` failing to enter download mode.
With the stub, `write_flash` erases what it writes, which is all that was
ever needed.

### The guard

`tools/check_serial_port.sh [/dev/ttyACM0]` lists every process with the port
open (PID, cwd, command) and exits non-zero if there is one. Run it before
`idf.py flash`, `idf.py monitor`, or any capture. It is the first thing to run
when esptool says the chip stopped responding or a capture comes back empty.

---

## 2. The solved problem: attaching a console resets the board

**Opening `/dev/ttyACM0` resets an ESP32-S3.** The tty layer asserts the modem
control lines on open and drops them on close; the USB-Serial-JTAG peripheral
turns that into a chip reset. The reset shows up in the log as:

```
rst:0x15 (USB_UART_CHIP_RESET)
```

Setting `dtr = False` and `rts = False` on a pyserial object *before* `open()`
does **not** prevent this — the kernel's `cdc_acm` driver sends
`SET_CONTROL_LINE_STATE` on its own.

### The fix

```sh
stty -F /dev/ttyACM0 -hupcl clocal
timeout 120 cat /dev/ttyACM0 > boot.log
```

Verified: 25 seconds attached, zero boot cycles, board undisturbed. With
`-hupcl` set, attaching to a running board leaves it running, and runtime
logging (driver traces, input events) arrives normally.

This is documented in `CLAUDE.md` under "Reading the console on a
USB-Serial-JTAG board".

### How badly this goes wrong if you miss it

A capture loop that reconnects when the port goes quiet will reset the board
on every reconnect. One such loop produced **48 boots in 190 seconds**, every
one of them `rst:0x15`, with no panic anywhere in the log — while someone was
trying to press keys on the device. The logs look convincing the whole time,
because a board that keeps rebooting keeps producing boot output.

Recognise it with:

```sh
grep -c '2nd stage bootloader' boot.log          # > 1 means you are resetting it
grep -o 'rst:0x[0-9a-f]* ([A-Z_]*)' boot.log     # USB_UART_CHIP_RESET means you
```

A genuine crash announces itself with a panic and a backtrace. `rst:0x15` on
its own is your own tooling.

### The part that is still lossy

With the `stty`/`cat` recipe, a boot capture stops at or before
`cpu_start: Multicore app`, where the application takes the USB peripheral
over from the ROM. The host does **not** see a USB disconnect there
(`journalctl -k` shows no re-enumeration); what differs is how the reader holds
the port. A reader that opens with DTR and RTS low receives the entire boot,
from `rst:0x15` through `cpu_start` to `ThistleOS ready`: `idf.py monitor`
(esp-idf-monitor 1.9.0) does, and so does `tools/serial_capture.py`. The
price is that such an open resets the board every time (three attaches, three
`rst:0x15` boots, 7821 bytes each). So:

- **Watch a running board without disturbing it**: `stty -F /dev/ttyACM0
  -hupcl clocal` then `cat`. No reset; runtime logging arrives; a boot would
  be truncated.
- **Capture a boot**: `tools/serial_capture.py boot.log --seconds 30`, or
  `idf.py monitor`. Attaching is the reset; the capture is complete.

Consequence: the boot burst is where log lines go missing on this board. Do not
put anything you need to read into it. This is why `main.c` reports the battery
and any driver-start failure at a quiet "boot health" milestone after
`ThistleOS ready`, rather than from inside driver init.

---

## 3. Two readers on one tty steal each other's bytes

If another process has `/dev/ttyACM0` open, your capture will silently receive
nothing, or a random subset. This happened here: another agent's
`serial_capture.py` held the port and consumed the board's entire console
output, while the device was demonstrably running and being typed on. The
symptom was **0 bytes and 0 boot cycles over ten minutes**, which is easy to
misread as a dead console or a dead board.

Find every holder:

```sh
for p in $(ls /proc | grep -E '^[0-9]+$'); do
  if ls -l /proc/$p/fd 2>/dev/null | grep -q ttyACM0; then
    echo "PID $p: $(tr '\0' ' ' < /proc/$p/cmdline)"
  fi
done
```

`tools/check_serial_port.sh` does the same and exits non-zero on a holder;
`fuser` and `lsof` also work when installed. Check this **first** whenever a
capture comes back empty — before theorising about DTR, cables or firmware. An
empty capture from this board is far more often a tooling problem than a device
problem.

---

## 4. A verified flash is not proof of what will boot

After one run of the chunked flasher reported `wrote 158/158 chunks, all
verified on-chip`, the board came up running something else entirely: an
ESP-IDF **v6.0.2** bootloader at `0x0`, a default `nvs` / `phy_init` /
`factory` partition table at `0x8000`, and an erased app region. Read back
three times in fresh connections, always the same.

The cause was mundane: **another agent flashed this board by mistake**,
believing it was working on a different device. But the diagnosis took a long
time because "the flasher said verified" was treated as settled.

The rule that came out of it: **verify by reading the flash back and diffing it
against the image**, not by trusting the flasher's report.

```sh
for off in 0x0 0x8000 0x20000 0x100000 0x260000; do
  d=$(printf "%d" $off)
  esptool.py --no-stub --chip esp32s3 --port /dev/ttyACM0 --after no_reset \
    read_flash $off 0x1000 /tmp/v.bin
  dd if=merged.bin of=/tmp/e.bin bs=4096 skip=$((d/4096)) count=1 2>/dev/null
  cmp -s /tmp/v.bin /tmp/e.bin && echo "MATCH $off" || echo "DIFFER $off"
done
```

Note that `read_flash` occasionally fails to produce a file at all on this
board (a dropped connection, not a mismatch). Retry a couple of times before
concluding anything, and check the file is non-empty — a missing file compared
against an expected block reports "DIFFER" and sends you chasing a phantom.

**If more than one agent or person shares this bench, confirm which device is
on which port before flashing.** Both incidents in this session — the mystery
image and the stolen console — came from two agents pointed at one board.

---

## 5. A build trap that looks like a flashing problem

Not USB, but it wasted a flash cycle and a wrong conclusion, so it belongs
here.

*Fixed; kept here because it explains old builds.* `idf.py build` used to
report success while linking against a *previous* run's Rust objects.
`build/esp-idf/kernel_rs/libkernel_rs.a` was found three hours older than the
Rust staticlib it is built from: the POST_BUILD step that merges the Rust
objects into the component archive only runs when CMake relinks that
component, and the `stub.c` touch meant to force it came after Ninja had
already read the timestamps, so it took effect only on the *next* build.
Edits under `crates/thistle-tk/` never reached the image at all, because the
Rust step watched only `components/kernel_rs/src/*.rs`.

A firmware was flashed, and a conclusion drawn from it, that did not contain
the fix under test.

`components/kernel_rs/CMakeLists.txt` now runs cargo on every build and lets it
decide what is stale, and makes `stub.c`'s object depend on the Rust library
file, so a new library relinks the archive and reruns the merge in the same
build. A single `idf.py build` after any Rust edit is enough.

---

## 6. Working recipes

### Flash and monitor

```sh
source ~/esp/v5.5.1/esp-idf/export.sh
tools/check_serial_port.sh /dev/ttyACM0      # must say "is free"
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

That is the whole recipe; it is the same as on every other board. If
`check_serial_port` lists a holder, stop it and re-run the check. Do not
proceed with a second reader on the port, and do not fall back to the chunked
flasher.

To verify what is in flash independently of the flasher's report, read back
and diff (a full 2.4MB read takes about a minute):

```sh
cd build
esptool.py --chip esp32s3 -p /dev/ttyACM0 --after no_reset \
  read_flash 0x20000 $(stat -c%s thistle_os.bin) /tmp/rb.bin
cmp /tmp/rb.bin thistle_os.bin && echo MATCH
esptool.py --chip esp32s3 -p /dev/ttyACM0 --before no_reset --after hard_reset run
```

### Watch a running board

```sh
stty -F /dev/ttyACM0 -hupcl clocal
timeout 300 cat /dev/ttyACM0 > run.log
```

### Capture a boot

```sh
tools/check_serial_port.sh /dev/ttyACM0
tools/serial_capture.py boot.log --seconds 30     # attaching resets the board
grep -c '2nd stage bootloader' boot.log             # expect exactly 1
```

Or `idf.py -p /dev/ttyACM0 monitor`. Both reset the board on attach and show
the complete boot. The passive `stty`/`cat` recipe is for watching a board you
must not disturb; it truncates a boot at `cpu_start` (§2).

---

## 7. What this cost, so the next person can skip it

The flashing "workaround" cost six minutes a cycle for two days and a
long list of hardware theories, for a problem that was one `ls -l /proc/*/fd`
away. The console problem cost more: most of a long session went into captures
that were resetting the board, then into captures that were silently losing to
another process.
Several wrong conclusions were published along the way and later retracted —
"SHL does nothing on this panel", "the charger needs a settling rail", "column
0 of the keyboard is dead". Every one of them came from reasoning on a log that
the tooling had corrupted.

If you take one thing from this document: **when this board misbehaves over
USB, suspect the tooling before the device.** Run `tools/check_serial_port.sh`,
check the boot-cycle count, and only then start forming a theory about the
hardware. Both of the "hardware" problems in this document were two agents
pointed at one board.
