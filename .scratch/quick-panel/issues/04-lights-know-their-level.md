# 04: Lights know their level

**What to build:** `light_prefs.rs` keeps the level the screen light and the key light are at and whether the board has each, and sets them for anyone who asks (`light_set_display`, `light_set_keys`). Settings' light buttons go through it.

**Blocked by:** none.

**Status:** done

- [x] `light_display_available`, `light_display_level`, `light_set_display(percent, save)`, and the same for the keys
- [x] A light is there until its driver answers `ESP_ERR_NOT_SUPPORTED`; boot settles it by setting each light to its saved level, or to the dark it starts in
- [x] Waking from sleep keeps the levels it restores
- [x] Settings' Display and keyboard pages set the lights through it
- [x] Host tests

## Comments

### Done

`set_display_now()` / `set_keys_now()` are the only places a light is set, and they record the level and whether the driver answered `NOT_SUPPORTED`. `light_prefs_restore()` no longer returns early with no system.json: it has to settle whether the lights are there either way. On e-paper with nothing saved it now sets the light to 0, which is where the driver starts it. On the keys, the same.

`restore_after_sleep()` now puts back the levels the lights were at before, kept here, rather than the saved ones; the power manager puts them out through the drivers directly, which leaves those levels alone. The Flashlight app, which lit the screen to 100 and "restored" it to 50, now goes back to `light_display_level()`.

### Confirmed

The user tried it on the T-Deck Max after the firmware above was flashed: it works.
