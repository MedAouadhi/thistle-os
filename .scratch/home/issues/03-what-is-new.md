# 03: What is new

**What to build:** Fill Home's empty band with at most two rows of what is new, each one tap from what it is about, read from kernel state only. With nothing new, "All caught up" and the mesh's line. Read `../spec.md` first; this ticket is written for an agent that has not seen the work so far.

**Blocked by:** 02 (done).

**Status:** open

## The rule that matters most

The user's own words: "don't do the Now registry with anything that changes the behavior of the apps or in a hacky way, or in a way that breaks the current modularity of the apps." So:

- No registry, no hook, no API an app implements or calls. The rows are a fixed set inside the launcher.
- Each row reads public, read-only kernel calls the status bar or the call chip already read, and a tap does what system UI already does: launch an app, or `rs_call_dial()`.
- No row for anything only an app knows: no podcast, no Hermes reply, no message text or sender name (counts only). The table below is the whole list.
- Never call `rs_msg_alert_take()` (it takes the pop-up the status bar shows), `rs_call_missed_seen()` (the Phone app's Recents clears the count), or anything that writes. Ask the phonebook only when a missed call needs a name: its first use starts a read of the SD card.

## The rows

Decided with the user through the mockups (https://claude.ai/artifact/EizLLcJZzzajZjo8dFrv1Q, artboards 1 and 2; the third row of the canvas is what ticket 02 built). Order: new messages, then a missed call, then the tailnet; the first two that apply are shown.

| Row | Shows when | Title / detail | Reads | Tap | Button |
|---|---|---|---|---|---|
| New messages | `rs_sms_unread_count() + rs_mesh_unread_count() > 0` | "2 new messages" / "1 SMS · 1 from the mesh" ("1 new message" / "SMS" or "From the mesh") | `thistle/sms.h`, `thistle/mesh.h` | launch `com.thistle.messenger`, as `statusbar.c`'s `messages_tap_cb` does | none |
| Missed call | `rs_call_missed_count() > 0` | "Missed call" / "Reza Amini · 13:52"; "2 missed calls" / "Last: Reza Amini" | `thistle/call.h`: the newest `CALL_LOG_MISSED` entry from `rs_call_log_get()` (index 0 is the newest; re-read only when `rs_call_log_seq()` changes); `rs_phonebook_name_for()`, else the number, else "Withheld" | launch `com.thistle.phone`, as `call_ui_chip_tapped()` does | call back: `rs_call_dial(number)`; none for a withheld number |
| Tailscale waiting | `tailnet_get_status()` says `TAILNET_NEEDS_LOGIN` or `TAILNET_NEEDS_APPROVAL` | "Tailscale" / "Sign in to join your tailnet" or "Waiting for an admin's approval" | `thistle/tailnet.h` | launch `com.thistle.tailscale` | none |
| All caught up | none of the above | "All caught up" / "No new messages or calls" | | not a button | none |
| Mesh on | none of the above, and `rs_mesh_is_running()` | "Mesh on" / "4 contacts" | `rs_mesh_get_contact_count()` | launch `com.thistle.messenger` | none |

The time in "13:52" is the call's `time` (UTC seconds; 0 if the clock was not set, then leave it out) in local time and the user's 12 or 24 hour format: `localtime_r()` agrees with the status bar (CLAUDE.md, "The clock"); `time_manager_format_time()` formats only now, so format the call's time the same way by hand.

## How to build it

- **Decide in a pure function, draw in `launcher_ui.c`.** Put the choice of rows and their text in `home_core.c` (or a new `home_now.c` beside it), fed a struct of the readings, with host tests in `simulator/tests/test_home_core.c` (registered in `simulator/tests/test_sim_main.c`; sources listed in `simulator/CMakeLists.txt` under `thistle_sim_tests`, and in `components/apps_builtin/CMakeLists.txt` for the firmware). That is how `home_layout()`, `home_pick_pins()` and `home_format_date()` are done now.
- **Where:** `home_layout_t.now_y` / `now_h` (app-area coordinates, below the 24 px status bar). On the 240 × 296 app area of the T-Deck Max: y 64 to 144, two rows of 40. On the 320 × 216 of the T-Deck LCD: y 48 to 112, two rows of 30, 4 apart, title and detail on one line. `now_h` is 0 on the T-Display, the Cardputer and the OLEDs: no rows there.
- **Build and delete with Home.** Home's widgets exist only while it is in front: `build_home()` makes them in `launcher_ui_show()`, `destroy_home()` deletes them in `launcher_ui_hide()`. Create the rows in `build_home()` between the clock and the grid (so Tab reaches them first), and set every pointer to them back to NULL in `destroy_home()`.
- **Poll, and touch only what changed.** One `lv_timer` at 1 s, created once like `s_clock_timer`, that does nothing while `s_home` is NULL; it reads the counts, and rebuilds the rows' text only when a reading changed, rewriting a label only when its text did (`set_text_if_changed()`). On e-paper every rewrite is a refresh. No countdowns or relative times ("5 min ago") that change by themselves.
- **A tap launches from `lv_async_call()`**, as `pinned_clicked_cb` does: launching hides Home, which deletes the row that was tapped, and deleting an object inside its own event is a use-after-free.
- **Styles:** shared `lv_style_t`s, initialised in `init_home_styles()` (`s_mono` is true on e-paper). Rows are `lv_button`s, so they join the keyboard's group; `lv_obj_remove_style_all()` them and their labels, or the theme's label colour stops a label following its button's pressed or focused colours. `tile_state_cb()` shows how a child mirrors its button's state. On e-paper only black and white: hierarchy from size (Montserrat 18 title, 14 detail) and inversion; `text_secondary` is black there. Glyphs are LVGL's: `LV_SYMBOL_ENVELOPE`, `LV_SYMBOL_CALL`, `LV_SYMBOL_OK`; `LV_SYMBOL_SHUFFLE` is Tailscale's icon in the drawer. LVGL has no dotted border; a 1 px rule between the rows is fine.
- **LVGL's pool is 64 KB** (CLAUDE.md). Keep a row to a handful of objects. In the simulator a pool run dry is a segfault in `lv_draw_add_task`, not a hang; `test_touch_taps.py`'s `eink-files-from-the-drawer-over-a-full-card` is the canary, and caught Home's first version.

## Tests to write and update

- Host tests for the pure function: each row's condition, the order, two at most, singular and plural, withheld numbers, a call with no time.
- A simulator test, `simulator/tests/test_home.py`, in the manner of `test_calls.py`, with a log line the launcher prints when its rows change (for example `(launcher_ui) now: 1 new message | All caught up`):
  - `--sms "Reza: hi"` on `tdeck-max`, and `--mesh-direct "Anna: hi"`: the messages row; a tap on it opens the Messenger.
  - `--call 0701234567` on `tdeck-max`, left to ring: it rings 3 s after the modem registers and gives up after 20 s, then the missed call row; its button dials (`(call_ui)` lines; `test_calls.py` has the strings); a tap opens Phone.
  - `THISTLE_SIM_TAILNET=login`: the Tailscale row (`test_tailscale.py` shows the setup).
  - Run each on a scratch copy of the card (`THISTLE_SIM_SDCARD`, as `test_touch_taps.py` and `test_file_transfer.py` do): the call log (`phone/calls.jsonl`) and the message history are kept on the simulator's card between runs, and a missed call left there by one run would show on the next one's Home.
  - Add it to `.github/workflows/tests.yml` beside the other `tests/test_*.py` steps.
- `simulator/tests/test_launcher_visuals.py`: `lcd-home` and `eink-lvgl-home` will draw the quiet rows, so their hashes change. Run them on a scratch card too, so they are deterministic, and check whether the simulated mesh's contact count is the same every run before hashing it; if it is not, mask the band as the clock's is (`clock_rows`). Positional `Case(...)` arguments: `wm` comes before `clock_rows`.
- `simulator/tests/test_touch_taps.py` taps Phone at (40,198) on the T-Deck Max and (40,170) on the T-Deck, and All apps at (120,302); the rows sit above those and should not move them.

## Commands

```sh
# simulator and its unit tests (run simulators one at a time: they share /tmp/thistle_sdcard)
cd simulator/build && cmake .. && make -j8 thistle_sim thistle_sim_tests && ./thistle_sim_tests
cd simulator && python3 tests/test_launcher_visuals.py && python3 tests/test_touch_taps.py
# a screenshot of Home
simulator/build/thistle_sim --headless --device tdeck-max --timeout 3000 --screenshot home.ppm
# firmware (never source ~/export-esp.sh in this shell)
. ~/esp/v5.5.1/esp-idf/export.sh && idf.py build
python3 tools/check_partition_sizes.py --partition-table partitions.csv --firmware build/thistle_os.bin
```

The other suites CI runs are listed in `.github/workflows/tests.yml`; all passed after 02 except one that already failed before it (comment on 02).

## Checklist

- [ ] The pure function and its host tests
- [ ] The rows in `launcher_ui.c`, built and deleted with Home, polled once a second
- [ ] Never `rs_msg_alert_take()`, `rs_call_missed_seen()`, nor the phonebook before a missed call needs a name
- [ ] Call back through `rs_call_dial()`; taps launch through `lv_async_call()`
- [ ] `test_home.py`, on a scratch card, and in CI
- [ ] `test_launcher_visuals.py` hashes, deterministic
- [ ] On the T-Deck Max: a message arriving refreshes the rows alone; the rows read on the panel
