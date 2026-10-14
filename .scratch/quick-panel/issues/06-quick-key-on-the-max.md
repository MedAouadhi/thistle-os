# 06: The T-Deck Max's middle key opens the panel

**What to build:** The CST66xx driver reports `HAL_KEY_QUICK` for the key its profile names `quick_key`, 1 by default; the T-Deck Max's profile names it. Then everything checked on the hardware.

**Blocked by:** 05.

**Status:** done

- [x] `quick_key` in the driver config (Rust and C), defaulting to 1
- [x] T-Deck Max profile, SD card and embedded copies
- [x] On hardware: drag opens, key opens and closes, each switch works and survives a reboot

## Comments

### Done

`quick_key` in `TouchCst66xxConfig` and `touch_cst66xx_config_t`, defaulting to 1 in `board_builtin_drivers.c`, as `home_key` and `back_key` default to 0 and 2, so a Max holding an older `board.json` gets the key without rewriting it. The SD card and embedded profiles name it too.

### On the hardware so far

Flashed to the T-Deck Max: `touch-cst66xx: ... home_key=0 back_key=2 quick_key=1`, both lights found and restored to 25%, MeshCore on air, `internal RAM 146 KB free, largest block 68 KB` as before. The drag, the key and the switches themselves need a hand on the board.

### Confirmed

The user tried it on the T-Deck Max after the firmware above was flashed: it works.
