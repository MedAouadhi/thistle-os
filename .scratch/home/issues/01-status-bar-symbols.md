# 01: The status bar shows symbols

**What to build:** The status bar shows the network that carries traffic as a symbol (WiFi's, Bluetooth's, or the mobile network's name, "LTE"), the battery as its percentage and a battery of as many bars, or a bolt while it charges. `statusbar_set_clock_visible()` lets Home leave the bar's clock out.

**Blocked by:** none.

**Status:** done in the simulator; not yet seen on hardware

- [x] Network from `net_get_active_underlay()`'s type: not the tailnet over it; nothing while there is none
- [x] `72% ` and `LV_SYMBOL_BATTERY_*`, `LV_SYMBOL_CHARGE` while charging, `--% ` and an empty battery when the driver cannot say
- [x] `statusbar_set_clock_visible()`
- [ ] Seen on the T-Deck Max over WiFi, over LTE, charging and on battery
