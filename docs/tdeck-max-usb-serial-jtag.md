# T-Deck Max: flashing and console over USB-Serial-JTAG

A handoff document. Everything here was established on real hardware between
2026-09-14 and 2026-09-15, on a LilyGo T-Deck Max (ESP32-S3, QFN56, rev v0.2,
MAC `80:45:6b:46:cf:20`, 16MB Winbond flash, 8MB PSRAM), host Linux with
ESP-IDF v5.5.1 and esptool v4.12.dev2.

The board has no UART header in use: its only console and its only flashing
path are the ESP32-S3's built-in USB-Serial-JTAG peripheral, on
`/dev/ttyACM0`. Both are unreliable, in two separate ways, and each has cost
a debugging session. **The flashing problem is unsolved and is the work this
document is handing over.** The console problem is solved; it is written up
because getting it wrong wastes hours and looks like a hardware fault.

---

## 1. The unsolved problem: full-speed flashing dies

`esptool write_flash` does not survive a normal flash of this board. It fails
a few KB in:

```
A fatal error occurred: The chip stopped responding.
```

with a `StopIteration` out of `esptool/loader.py:421` in `slip_reader`,
meaning the SLIP stream went silent mid-transfer.

Reproduced at 460800 baud with the stub, `--before default_reset --after
hard_reset`, writing a 2.5MB merged image to `0x0`. The shape of the failure
is consistent: any single flash operation lasting more than roughly 1–3
seconds kills the link.

### What is known to work

`../T-Deck-MAX/tools/flash-usb.sh` (outside this repo, in the vendor
checkout). It writes the image in 16KB chunks, batching as many as the chip
accepts per connection and advancing by however many actually verified:

```sh
esptool.py --no-stub --chip esp32s3 --port /dev/ttyACM0 --baud 115200 \
  --after no_reset \
  write_flash --flash_mode keep --flash_freq keep --flash_size keep \
  --no-compress 0x0 chunk0 0x4000 chunk1 0x8000 chunk2 0xc000 chunk3
```

Each chunk is verified on-chip by the ROM's flash-MD5 command (esptool prints
`Hash of data verified`). A full image is 158 chunks and takes about six
minutes. The board is left in download mode and needs a physical USB replug
to boot.

### What has been ruled out

- **The cable.** Replacing it changed nothing: full-speed flashing failed
  identically afterwards. (It was a reasonable suspicion and is now closed.)
- **Flash health.** Writes persist reliably. Test patterns written at
  `0x700000` and `0x20000` survived disconnect and reconnect and read back
  byte-for-byte.
- **Chunk verification being a lie.** The per-chunk MD5 is genuine; when a
  whole image later failed to match, the cause was an unrelated third party
  (see §4), not a false verify.

### What has not been tried

These are the obvious next experiments, roughly in order of expected value:

1. **`--no-stub` versus the stub.** The workaround uses `--no-stub` *and*
   small chunks *and* 115200 baud *and* `--no-compress`. Nobody has taken
   those apart. Find the single variable that matters — try the stub with
   16KB chunks, and `--no-stub` with a full-size write, and so on.
2. **`idf.py monitor` / `esp-idf-monitor`.** Espressif's own tooling knows
   about USB-Serial-JTAG and may already handle whatever this is. Untested
   here.
3. **Host-side USB.** A different port, a powered hub, or a different host
   controller (xHCI vs. others). USB-Serial-JTAG is sensitive to host timing
   and this has not been varied at all.
4. **Power.** Whether the board is running from USB alone or with a charged
   battery, and whether the failure correlates. The charger (SY6970) is live
   during flashing and was observed drawing 250 mA.
5. **ESP32-S3 errata.** Check Espressif's USB-Serial-JTAG errata and issue
   tracker for this silicon revision (v0.2) before assuming the board is at
   fault.
6. **`esptool --after no_reset` accumulation.** The workaround notes the chip
   "completes roughly two such operations per connection before it needs a
   reset". Understanding *why* would likely explain the whole failure.

### Related: `erase_region` does not work with `--no-stub`

```
A fatal error occurred: Failed to enter Flash download mode
```

Workaround: `write_flash` erases the sectors it writes, so writing a block of
`0xFF` over the region has the same effect. This was used to blank 16KB at
`0xBA0000` to force a SPIFFS reformat.

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

Everything between `cpu_start: Multicore app` and the host re-attaching is
lost, because that is where the application takes the USB peripheral over from
the ROM and the host briefly sees a disconnect. Every boot capture in this
session stopped at exactly that line.

To capture a full boot you must attach first, never reopen, and power-cycle by
**physically replugging**. `esptool --after hard_reset` will not serve, because
esptool's own connection resets the chip again on the way out.

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

`idf.py build` can report success while linking against a *previous* run's Rust
objects. `build/esp-idf/kernel_rs/libkernel_rs.a` was found three hours older
than the Rust staticlib it is built from: the POST_BUILD step that merges the
Rust objects into the component archive only runs when CMake relinks that
component, and the `stub.c` touch meant to force it happens too late in an
incremental build.

A firmware was flashed, and a conclusion drawn from it, that did not contain
the fix under test.

```sh
touch components/kernel_rs/stub.c && idf.py build
```

If the result matters, verify with `ar p` and `cmp` on an archive member rather
than trusting timestamps. The real fix — making the merge depend on the Rust
library file — is still open.

---

## 6. Working recipes

### Flash and verify, end to end

```sh
source ~/esp/v5.5.1/esp-idf/export.sh
touch components/kernel_rs/stub.c && idf.py build

cd build && esptool.py --chip esp32s3 merge_bin -o /tmp/merged.bin \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0xf000 ota_data_initial.bin \
  0x20000 thistle_os.bin

bash ../T-Deck-MAX/tools/flash-usb.sh /tmp/merged.bin /dev/ttyACM0
# then verify by read-back (§4), then physically replug to boot
```

Do **not** pass `ESPTOOL="python -m esptool"` to that script: it invokes
`"$ESPTOOL"` as a single command name, so a multi-word value fails with
`command not found` and the script reports `stalled at chunk 0`, which looks
like a hardware stall. Let it use `esptool.py` from the IDF environment.

### Watch a running board

```sh
stty -F /dev/ttyACM0 -hupcl clocal
timeout 300 cat /dev/ttyACM0 > run.log
```

### Capture a boot

Attach as above **first**, then physically replug the USB cable. Accept that
the window around `cpu_start` will be missing.

---

## 7. What this cost, so the next person can skip it

The flashing workaround is tolerable at six minutes a cycle. The console
problem was not: most of a long session went into captures that were resetting
the board, then into captures that were silently losing to another process.
Several wrong conclusions were published along the way and later retracted —
"SHL does nothing on this panel", "the charger needs a settling rail", "column
0 of the keyboard is dead". Every one of them came from reasoning on a log that
the tooling had corrupted.

If you take one thing from this document: **when a capture from this board is
empty or strange, suspect the tooling before the device.** Check for other
holders of the port, check the boot-cycle count, and only then start forming a
theory about the hardware.
