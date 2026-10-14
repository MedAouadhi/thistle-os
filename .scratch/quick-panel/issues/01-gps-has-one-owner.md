# 01: GPS has one owner

**What to build:** The kernel owns whether GPS is on (`gps_manager.rs`, `thistle/gps_manager.h`), saves it as `thistle_os.gps.enabled`, and restores it at boot. Settings' GPS page and the `thistle_gps_enable` syscall go through it.

**Blocked by:** none.

**Status:** done

- [x] `rs_gps_set_enabled(bool)` applies at once and saves; `rs_gps_is_enabled()`; `rs_gps_available()`
- [x] Restored at boot once the drivers start; on unless saved off
- [x] `thistle_gps_enable` syscall goes through it, unsaved
- [x] Settings' GPS page shows the real state and switches through it
- [x] Host tests for reading the setting

## Comments

### Done

`gps_manager.rs` keeps whether the reader is running in an `AtomicBool`, set only when the driver's `enable`/`disable` returned OK, so it never says on for a receiver that would not start. `kernel_boot.rs` calls `gps_manager::restore()` right after the lights. Settings' GPS page lost its own `s_gps_enabled`, which started out `true` while the driver started with its reader stopped, so its button said "Disable" on a receiver that was off.

### On unless switched off

First written as off unless switched on, from the driver, whose reader starts stopped. The first boot on the Max said `Starting GPS...`: the HAL registry enables the reader at boot on purpose (5684319, "Start the GPS receiver at boot"), so the manager said off about a receiver that was on. It now defaults to on, and `restore()` applies the saved choice either way; enabling a running reader is a no-op in the driver that records the state.

### Confirmed

The user tried it on the T-Deck Max after the firmware above was flashed: it works.
