# 01: The microphone key reaches the window manager

Status: in-review

**What to build:** The T-Deck Max's microphone key goes down and comes up as
`HAL_KEY_MIC` (0x0103), a key that types nothing, as Home, Back and the
quick key are. The TCA8418 driver swallowed it until now. Under Sym or Alt
it still types `0`, its printed second legend. The LVGL manager keeps it
away from LVGL's keypad and hands both edges to dictation (ticket 02). The
simulator's F4 is the mic key, and `--tap mic` presses it for `--tap-hold`
milliseconds.

- [x] `HAL_KEY_MIC` in `hal/input.h`, mirrored in `hal_registry.rs`
- [x] TCA8418: the mic key sends `HAL_KEY_MIC` down and up; under the symbol layer, `0`
- [x] The release names the code the press sent, as for every key
- [x] `manager.c` never passes it to LVGL
- [x] Simulator: F4, and `--tap mic`
- [x] Host tests for the driver's decode

## Comments

Four driver tests changed or added: the key goes out as `HAL_KEY_MIC` down
and up; a letter typed while it is held does not end it; Sym let go while it
is down still releases the `0` it typed. The 0x04 code itself still never
leaves the driver. thistle-tk passes unknown codes through `dispatch_key`
unhandled, as it does `HAL_KEY_QUICK`. `--tap mic:MS` holds it for MS.

Not yet seen on the board.
