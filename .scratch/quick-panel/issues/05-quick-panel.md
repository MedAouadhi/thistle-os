# 05: The quick panel

**What to build:** A panel that drops below the status bar when the user drags down from it, with switches for WiFi, Mobile data, GPS, LoRa, Key light and Mute, sliders for Screen light and Volume, and a way into Settings. Back, Esc, Home, a tap below it or a drag up close it. Built on the registry in the spec, under both LVGL window managers.

**Blocked by:** 01, 02, 03, 04.

**Status:** done

- [x] `pull_gesture.c` with host tests
- [x] `quick_panel.c`: registry, drawing, 1 s state refresh that touches only what changed, keyboard focus
- [x] `quick_panel_tiles.c`: the built-in switches and levels
- [x] UI manager: gesture, `HAL_KEY_QUICK`, Back/Esc/Home close it first
- [x] Simulator: `--drag`, `--tap quick`; `test_quick_panel.py`; screenshots on tdeck-max and tdeck
- [x] Firmware builds

## Comments

### Done

- `pull_gesture.c`, 16 host tests in `simulator/tests/test_pull_gesture.c`.
- `quick_panel.c`: the panel hangs from the status bar on a clear backdrop. Tiles are 44 px high, as many 70 px-or-wider columns as fit (3 on the Max, 4 on the T-Deck). Styles are shared (`lv_obj_remove_style_all` on everything, then the panel's own), so a dozen tiles cost little of the 64 KB pool. While open the keypad has a group of its own, so keys never reach the app underneath. It closes asynchronously (`lv_obj_delete_async`), since a button on it may be what asked.
- `quick_panel_tiles.c`: WiFi, Mobile, GPS, LoRa, Key light, Mute; Screen light and Volume.
- UI manager: the touch stream feeds the gesture; `HAL_KEY_QUICK` toggles; Back, Esc and Home close the panel first.
- Simulator: `--drag X1,Y1,X2,Y2`, `--tap quick`, F2 in the window. `simulator/tests/test_quick_panel.py`, 10 cases, puts `simulator/sdcard/config/system.json` back after each (the simulator's SD card is that directory, in the repository).

### Seen in the simulator

On the tdeck-max (e-paper, 240 x 320): five tiles in two rows of three (no key light: the simulated keyboard has none), both sliders, Settings and close. On the tdeck (LCD, 320 x 240, dark theme): four tiles in one row, no scrolling. The panel needs 212 of the 216 px below the bar there; a seventh switch on a landscape board puts it into a scroll, and the drag up that closes it would then compete with scrolling.

### Tests

Rust kernel 1814; simulator unit 74 (16 new); integration 18/18; launcher visuals, app matrix (102), touch taps, fits screen, keyboard typing, Assistant, quick panel (10): all pass. `test_epaper_partial_flush.py`'s `eink-settings-values` fails, with the same two screen hashes, on HEAD built without any of this: the Mobile data value moves from "Starting for SMS" to "SMS only" between its two captures. Not this ticket's, but CI's.

### Confirmed

The user tried it on the T-Deck Max after the firmware above was flashed: it works.
