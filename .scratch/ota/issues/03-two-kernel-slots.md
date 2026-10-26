# 03: Two kernel slots

**What to build:** The partition table becomes two 4.5 MB kernel slots and a
2.5 MB reserved slot, with NVS, otadata, SPIFFS and the core dump where they
were (the table is in the spec). `idf.py flash` writes the first kernel slot,
as it wrote `factory` before. The kernel always names the other kernel slot by
its label, never ESP-IDF's next update partition, so the existing card update
(a `.bin` and its `.sig` in `/sdcard/update/`) now installs into the other
kernel slot instead of overflowing Recovery's. A new kernel is kept once it
has run for 60 s: until then a reset rolls it back. On a boot whose image is
pending, the power manager starts after the keep; on every other boot nothing
waits. The simulator gains kernel slots so the flow can be tested without
hardware. Spec: `../spec.md`, "The partition table", "The update manager"
(keeping a new kernel) and "Simulator". ADR-0010.

**Blocked by:** None (can start immediately)

**Status:** done in code; hardware pass pending

- [ ] On the T-Deck Max, one USB flash moves the board to the new table;
      WiFi, the mesh identity, `board.json` and `system.json` survive, and it
      boots the first kernel slot.
- [ ] A card update from either kernel slot writes the other one, never the
      reserved slot, and the device boots it after a restart.
- [ ] A reset within the first minute of a new kernel rolls back to the
      previous one; after the minute the new one is kept through resets.
- [ ] The boot log shows the power manager starting after the keep on a new
      kernel's first boot, and at once on any other boot.
- [x] In the simulator, kernel slots are files in a directory, a restart
      boots the selected slot's version, a run that ends before the keep
      leaves the next run on the other slot, and the keep delay is shortened
      by an environment variable; a simulator test covers install, restart
      and rollback.
- [x] CI's partition size check measures the kernel against a kernel slot.
- [x] The partition description in CLAUDE.md and the docs matches the new
      table.

## Comments

**2026-10-26, implemented with no board attached.** What is in:

- `partitions.csv` is the spec's table: `kernel_a` (ota_0), `kernel_b`
  (ota_1), `recovery` (ota_2, reserved), everything else where it was. The
  firmware builds; `build/flash_args` writes the kernel at 0x20000
  (`kernel_a`) and `ota_data_initial.bin` at 0xF000. The kernel is 3.86 MB,
  81 % of a kernel slot. ESP-IDF warns that it overflows `recovery`, and that
  `recovery` is also a data subtype's name; both are expected.
- `kernel_slot.rs` decides, host-tested: the other kernel slot by label
  (none from `recovery` or an old `factory`), when a new kernel is kept,
  the version an image's `esp_app_desc_t` says, and the simulator's model of
  the bootloader's otadata (new, pending, rolled back, kept).
- `ota.rs` runs them. Every ESP-IDF call is in
  `components/kernel/src/kernel_slot_platform.c`; the slot is selected only
  after the signature verifies and `esp_ota_end()` validates the image.
  `ota_get_running_partition()` now returns the real label (it read the
  `esp_partition_t` at the wrong offset before).
- `ota_keep_new_kernel(power_manager_start)` replaces the 5 s soak,
  `ota_mark_valid()` and the bundle cleanup in `main.c`: a kworker job keeps
  the kernel after 60 s, cleans up, then starts the power manager; on any
  other boot it starts at once.
- The simulator: `THISTLE_SIM_SLOTS` (default `/tmp/thistle_slots`),
  `THISTLE_SIM_KEEP_S`, `THISTLE_SIM_PUBLISHER_KEY` (a public key trusted
  for firmware in simulator builds only, which ticket 06 can build on), and
  `--install-update`. It starts no power manager, so it keeps with no
  callback. `simulator/tests/test_kernel_slots.py`: 6 cases, all pass.
- `tools/check_partition_sizes.py` measures the kernel against a kernel slot
  and Recovery against `recovery`; `tools/test_check_partition_sizes.py`
  also pins the table. `build.yml` runs it.

Left for the hardware pass (unticked above): the USB migration keeping
WiFi, the mesh identity, `board.json` and `system.json` and booting
`kernel_a`; a card update from each slot into the other; a reset in the first
minute rolling back and none after it; and the boot log's order of
`kept the kernel in kernel_b after 60 s; starting the power manager` before
the power manager's own line. Note that on a pending boot the power manager
starts on the kernel worker rather than the main task.
