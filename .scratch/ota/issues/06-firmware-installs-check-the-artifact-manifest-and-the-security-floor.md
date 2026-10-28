# 06: Firmware installs check the artifact manifest and the security floor

**What to build:** A release copied to the card (its `.bin`, artifact manifest
and the manifest's signature in `/sdcard/update/`) installs from Settings →
System update, and anything else is refused with the reason shown. A
firmware is accepted only if the manifest's signature verifies against the
kernel's key, its type is firmware, its arch is the running chip's, the
stored board id is among its compatible boards, its security version is at
least the device's security floor, and the bytes written match its size and
hash. Each kernel slot's version and security version are recorded in NVS
when it is written, and by a kernel for its own slot when it runs; the floor
rises when a new kernel is kept, so one rolled back never raises it. The
manifest's parsing and device checks come out of Recovery's module into a
shared crate the kernel uses; the decision is `update_policy`'s, pure and
tested on the host. This replaces the card update's detached `.sig` check.
Spec: `../spec.md`, "The artifact manifest", "The update manager", "Device UI"
and "Simulator". ADR-0010.

**Blocked by:** 03 (Two kernel slots), 04 (Release the firmware by tag)

**Status:** done

- [x] The shared crate carries the canonical-format tests Recovery's module
      has; Recovery itself is not changed.
- [x] `update_policy`'s accept decision is tested on the host: bad signature,
      wrong type, wrong chip, board not listed, below, at and above the
      floor; a card install may be any version at or above the floor.
- [x] Settings → System update shows the running version, Install from card
      when the card holds a release, progress, Restart now / Later, and the
      reason for a refusal; Settings' old SD apply is gone.
- [x] The simulator trusts a test publisher key named by an environment
      variable, in simulator builds only.
- [x] A simulator test installs from the card and refuses a bad signature, a
      wrong board, a wrong chip, a version below the floor and a size
      mismatch, each with its reason in Settings and the log; the floor rises
      only after the keep, and not for a run ended before it.
- [ ] On the T-Deck Max, a release candidate cut after this ticket installs
      from the card, and a copy with one byte changed is refused.

## Comments

**2026-10-26, implementer.** Done with no board attached; nothing pushed,
no tag created.

- **The shared crate is `crates/thistle-artifact`** (no dependencies): the
  canonical V1 manifest, `parse`, `canonical_bytes`, `validate_for_device`,
  with Recovery's four tests carried over. It accepts what Recovery's copy
  does not: a prerelease version (`0.6.0-rc.1`; a hyphen in it, or build
  metadata, is still refused), a firmware destination of `kernel` (and no
  longer `ota_1`), a plain-HTTP URL, and the arch `host`, the simulator's.
  Recovery and its CI step are unchanged; `tests.yml` also runs the crate's
  tests with `rustc --test`, as it does Recovery's module.
- **`update_policy.rs`** (16 host tests): `accept(signature_ok, manifest,
  device, source)` checks, in order, the signature, the canonical form, the
  type, the chip, the board, the floor, and for an online offer (ticket 07's)
  a version newer than the running one; a card install takes any version at
  or above the floor. `check_size`, `check_hash`, `floor_after_keep` and
  `pick_card_release` beside it; each `Refusal` has the `reason()` Settings
  and the log show. Ticket 07, 08 and 09 add their decisions here.
- **`update_manager.rs`** runs the card install on a FreeRTOS task with a
  16 KB internal stack (a thread in the simulator), holding the device
  awake: it picks the release in `/sdcard/update/` whose manifest names the
  newest version (`<name>.bin`, `.bin.manifest`, `.bin.manifest.sig`, as the
  release assets are named), verifies the manifest with the kernel's key only
  (`signing::verify_firmware`), accepts it, checks the size, streams the file
  into the other kernel slot hashing as it writes, checks the SHA-256, lets
  ESP-IDF validate and select it, and records the slot. The C API is
  `thistle/update.h` (`rs_update_get_status`, `_running_version`,
  `_card_release`, `_install_from_card`, `_restart`); ticket 07 adds to it.
  `ota_apply_from_sd()` is now a blocking wrapper of the same install, used
  by `--install-update` and by the App Store's Firmware tab until ticket 07
  removes it (that tab's downloads have no manifest, so they are refused
  now). The detached `.sig` check of the card update is gone.
- **NVS**: `components/kernel/src/update_platform.c`, namespace `thistle_sec`:
  the floor `fw_sec_ver` (u32, Recovery's) and `kernel_a_ver`/`kernel_a_sec`,
  `kernel_b_*`. A slot is recorded when written and by its kernel at boot
  (`kernel_booted`, from `ota_init`); the floor rises in `kernel_kept`, from
  the keep job, after `esp_ota_mark_app_valid_cancel_rollback`. The boot log
  says `security floor N; this kernel's security version M`. A device
  flashed by USB has floor 0 until its first kept update.
- **The stored board id** is now kept by `board_config` from the profile's
  `board.board_id`; the simulator names it with `board_config_set_board_id`
  (its `--device` name).
- **Settings → System update** (a new main-list row): the running version,
  what the card holds, Install from card, progress, the result or the
  refusal's reason, Restart now / Later. The About screen's old SD apply
  (which took a missing signature) is gone; the boot toast points at System
  update.
- **The simulator's test key** (`THISTLE_SIM_PUBLISHER_KEY`) is now read in
  simulator builds only (`feature = "sim-bus"`), not in host tests.
- **Tests:** `cargo test` 2043 passed; the crate's 9; `test_kernel_slots.py`
  (now signing manifests) 6 of 6; `test_system_update.py` 11 of 11: Settings
  shows the card's release, install and Restart now, Later, the floor rising
  only at the keep and not for a run ended before it or rolled back, an
  older release from the card, and refusals for a bad signature, another
  board, another chip, below the floor, a size mismatch and one byte
  changed, each in Settings and the log.

`idf.py build` succeeds. `test_epaper_partial_flush.py`'s
`eink-settings-values` fails on this machine with `ota`'s own Settings too:
the T-Deck Max's Mobile data row says `Starting` at 500 ms and `Calls & SMS`
at 3.5 s, so it is not this ticket's.

**Left for the user:** the T-Deck Max box, unticked. Cut a release
candidate after this ticket (`v0.6.0-rc.1` or later), copy its `.bin`,
`.bin.manifest` and `.bin.manifest.sig` into the card's `update/`, install
from Settings → System update, Restart now, and check the boot log's
`security floor 0; ...`, then `security floor raised from 0 to 500` a
minute later; then a copy with one byte changed should be refused with
`the file's SHA-256 is not its manifest's`. A device needs the ticket 03 USB
flash first.

