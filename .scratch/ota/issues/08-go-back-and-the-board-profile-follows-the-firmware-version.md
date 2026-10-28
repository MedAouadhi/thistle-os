# 08: Go back, and the board profile follows the firmware version

**What to build:** Two things that let a firmware version and its board
profile travel together. First, the stored board profile records the
firmware version that wrote it; when a kernel boots and that version is not
its own, and it embeds a profile for the stored board id, it rewrites the
stored profile from its embedded copy and keeps the previous one as a single
backup. A board id it has no profile for keeps its profile. This ends the
"changed profile never reaches a board that has one" trap tickets 03, 06, 09
and 10 of the T-Deck Max hit, for USB flashes and updates alike. Second,
Settings → System update offers Go back to the previous version, with no
download, while the other kernel slot's record matches that slot's own image
description and its security version is at or above the floor; going back
boots that slot, and its kernel brings back its own board profile. Spec:
`../spec.md`, "The update manager". ADR-0010.

**Blocked by:** 06 (Firmware installs check the artifact manifest and the security floor)

**Status:** done

- [x] `update_policy` decides, tested on the host, whether the board profile
      is rewritten (writer version equal, different, missing; board id with
      and without an embedded profile) and whether Go back is offered (record
      matching and not matching the image, below and at the floor).
- [x] Simulator tests: a version change rewrites the profile and leaves a
      backup; a custom board id keeps its profile; Go back boots the other
      slot and the profile follows it; Go back disappears once a new install
      starts erasing the other slot.
- [x] On the T-Deck Max, a laptop build with a changed embedded profile
      reaches the board on its first boot, with the old profile kept beside
      it, and system.json, WiFi and the mesh identity untouched.
- [ ] On the T-Deck Max, after an update, Go back returns to the previous
      version.
- [x] CLAUDE.md's "A changed board profile does not reach a board that has
      one" section says what happens now.

## Comments

**2026-10-26, implementer.** Done with no board attached; nothing pushed,
no tag created.

- **The writer version** is a top-level `"written_by": "<version>"` in the
  stored `board.json`, put in when the file is written, so the embedded
  copies and the catalog files stay equal for `validate_board_catalog.py`.
  `update_policy::board_profile(writer, running, embedded)` decides: the
  running version keeps it; any other, or none, rewrites it, if the kernel
  embeds a profile for the board id; else it is kept.
  `board_config::follow_firmware_version` does the file side: the previous
  profile goes to `board.json.bak` (one backup, replaced each time), then the
  embedded copy, stamped, to `board.json`. A profile that is the embedded
  copy as JSON with no stamp (just written by `board_detect_and_write()`) is
  only stamped, with no backup. It runs in `load_config` before the
  power-section patch, which stays for a board whose profile is not built in.
  The log says `board profile tdeck-max rewritten from 0.6.0's built-in copy
  (it was 0.5.2's); the previous one is board.json.bak`.
- **The simulator** reads no profile to set up its board, so it follows the
  version only after `ota_init` (it knows its slot's version then), on
  `THISTLE_SIM_SPIFFS/config/board.json`; a device does it in
  `board_config_init`, before drivers load, with the build's version.
- **Go back:** `update_policy::go_back(record, image, floor)` offers the
  other slot's version while its NVS record names the version its image
  description says (`kslot_platform_image_version`,
  `esp_ota_get_partition_description`) and its security version is at or
  above the floor. `update_manager` clears the other slot's record before
  `SlotWriter::begin` erases it, so Go back is gone from then on whether or
  not the install finishes; it is also withheld while an install waits for
  its restart. `rs_update_go_back()` selects the slot as it is
  (`kslot_platform_select`, `esp_ota_set_boot_partition`, which checks the
  image) and restarts; the old kernel boots new and is kept after a minute
  like an update, or rolled back by a reset before then. The simulator's
  otadata now records an erase (`erased=kernel_a`) and a select.
- **C API additions** (additive): `rs_update_go_back_version()`,
  `rs_update_go_back()` in `thistle/update.h`. Settings → System update has
  a Go back to `<version>` row after Later, hidden while an install runs;
  the log says `System update: Go back to X` and `System update: going back
  to X is no longer offered`.
- **Tests:** `cargo test` 2059 passed (update_policy 7 new, board_config 7
  new, kernel_slot 2 new); `test_system_update.py` 16 of 16, five new: a
  version change rewrites the profile and keeps the old one, a board with no
  built-in profile keeps its own, Go back boots the other slot and the
  profile follows it (and then offers going forward again), Go back is not
  offered below the floor, and Go back survives an install refused before
  the erase but disappears once one is refused after it.
  `test_kernel_slots.py` 6 of 6. `idf.py build` succeeds.
- `test_epaper_partial_flush.py`'s `eink-settings-values` now passes: each
  run uses a scratch card with the modem off, so the Mobile data row says
  Off in both captures (a separate commit).

Note for the hardware pass: a hand edit to `board.json` (another LoRa band,
say) lasts until the next version boots, and is then in `board.json.bak`.

**Left for the user:** the two T-Deck Max boxes. For the first, flash a
laptop build whose embedded tdeck-max profile differs from the board's and
check the boot log's `board profile tdeck-max rewritten from ...`, that
`/spiffs/config/board.json.bak` holds the old profile (File Transfer shows
SPIFFS), and that WiFi, settings and the mesh identity are as they were. For
the second, after a card install of a release with the same security version
as the running one and a minute's keep, Settings → System update should show
Go back to the previous version; tap it and check `running kernel_a,
version ..., new` and the profile rewritten back. A release with a higher
security version raises the floor at its keep, after which Go back is not
offered, as intended.

**Hardware, 2026-10-26 (by the user):** the first boot of `0.5.2-201-g8addbb4`
logged `board profile tdeck-max rewritten from 0.5.2-201-g8addbb4's built-in
copy (it was an unnamed version's); the previous one is board.json.bak`;
WiFi, system.json and the mesh identity were untouched.
