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

**Status:** ready-for-agent

- [ ] On the T-Deck Max, one USB flash moves the board to the new table;
      WiFi, the mesh identity, `board.json` and `system.json` survive, and it
      boots the first kernel slot.
- [ ] A card update from either kernel slot writes the other one, never the
      reserved slot, and the device boots it after a restart.
- [ ] A reset within the first minute of a new kernel rolls back to the
      previous one; after the minute the new one is kept through resets.
- [ ] The boot log shows the power manager starting after the keep on a new
      kernel's first boot, and at once on any other boot.
- [ ] In the simulator, kernel slots are files in a directory, a restart
      boots the selected slot's version, a run that ends before the keep
      leaves the next run on the other slot, and the keep delay is shortened
      by an environment variable; a simulator test covers install, restart
      and rollback.
- [ ] CI's partition size check measures the kernel against a kernel slot.
- [ ] The partition description in CLAUDE.md and the docs matches the new
      table.
