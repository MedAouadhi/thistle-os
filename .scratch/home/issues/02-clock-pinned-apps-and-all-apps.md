# 02: The clock, the pinned apps and All apps

**What to build:** Home's fixed bands (spec): the clock and the date, room kept for what is new, the pinned apps and a bar that opens All apps. The bands, the choice of apps to pin and the date's text in `home_core.c`, host-tested; drawing in `launcher_ui.c`.

**Blocked by:** 01.

**Status:** done in the simulator; not yet seen on hardware

- [x] `home_core.c` and `simulator/tests/test_home_core.c`: the bands on every simulated screen, the pins, the date
- [x] The clock font, `launcher/tools/gen_clock_font.sh`: 44 px, 4 bpp and 1 bpp
- [x] Pinned: Phone, Messenger, Hermes, Podcasts, Notes, Reader; a missing one's place goes to the next installed app that is not a tool
- [x] Tiles in the default group: focus inverts on e-paper, rings in colour; a press inverts or lights
- [x] Home built on show and deleted on hide; a pinned app launches once its click is over
- [x] The status bar's clock hidden while Home shows its own, shown over the app drawer
- [x] `test_launcher_visuals.py`: Home on the T-Deck and the T-Deck Max, clock band masked; `test_touch_taps.py` at the new positions
- [x] Firmware builds with ESP-IDF 5.5.1; 3,734,240 bytes, 79% of ota_1 (the clock fonts cost about 10 KB)
- [ ] On the T-Deck Max: a minute's change refreshes the clock's band alone; focus moves; taps open first time

The band for what is new is empty until 03.

## Comments

**What was built, and where.**

- `components/apps_builtin/launcher/home_core.{c,h}`: `home_layout()` (the bands for an app area of a given size), `home_pick_pins()`, `home_format_date()`. Cells span the whole width (80 px at 240 and at 320): "Messenger" is 78 px in Montserrat 14 and did not fit inside 8 px margins. The All apps bar keeps `HOME_MARGIN`.
- `components/apps_builtin/launcher/launcher_ui.c`: `build_home()` / `destroy_home()`, `create_clock()`, `create_pinned_grid()`, `create_pinned_tile()`, `create_apps_bar()`, `init_home_styles()`. The drawer and the Tools folder are as they were.
- `components/apps_builtin/launcher/fonts/home_clock_44{,_mono}.c` from `launcher/tools/gen_clock_font.sh` (Montserrat Medium from `managed_components/lvgl__lvgl/scripts/built_in_font/`, digits, colon, dash, space, A, M, P; line height 31 px).
- `components/ui/src/statusbar.c`: the symbols, and `statusbar_set_clock_visible()`. The launcher's title is now "" (`launcher_app.c`).
- Tests: `simulator/tests/test_home_core.c` (15), new cases and hashes in `test_launcher_visuals.py`, new tap points in `test_touch_taps.py`.

**What the first version got wrong, so the next does not.**

- Home's widgets stayed alive under the app opened from it, and `test_touch_taps.py`'s Files over a card of twenty folders ran LVGL's pool dry: a segfault in `lv_draw_add_task` in the simulator. Home is now deleted on hide and rebuilt on show, and a pinned app launches from `lv_async_call()` so its tile is not deleted inside its own click.
- A label sized to its content and given `LV_LABEL_LONG_DOT` drew "..." only: the names are measured with `lv_text_get_width()` and given a width.
- The theme puts a text colour on every label, so a label inside a button did not follow the button's pressed colours: Home's labels have `lv_obj_remove_style_all()`, and `tile_state_cb()` copies a tile's pressed and focused states to its box and name (LVGL 9.2 has no state-changed event; it sets the state before the user's callbacks run).
- The colour focus ring sits outside the icon box and was clipped by the tile, then by the grid: the tile reports `FOCUS_ROOM` of extra draw size (`LV_EVENT_REFR_EXT_DRAW_SIZE`) and the grid has that much padding above and below.
- A new field put before `wm` in `test_launcher_visuals.py`'s `Case` took the thistle-tk cases' positional `"thistle-tk"`: new fields go last.

**Tests after this ticket.** All pass except `test_epaper_partial_flush.py`'s `eink-settings-values`, which fails the same way, with the same hashes, on 9f6f04f without these changes: Settings' Mobile data value reads "Starting" at 500 ms and "Calls & SMS" at 3.5 s as the simulated modem comes up, so the screen differs for a reason the test does not mean to catch. Not fixed here.

**Not seen on hardware.** Nothing of 01 or 02 has been flashed. CLAUDE.md has the rules for the T-Deck Max's port (`tools/check_serial_port.sh`) and the board.json that a flash does not replace; Home does not depend on board.json.
