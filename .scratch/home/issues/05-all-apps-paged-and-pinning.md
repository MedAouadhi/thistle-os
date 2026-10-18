# 05: All apps paged, and pinning

**What to build:** All apps in alphabetical order, paged on e-paper rather than scrolled, with a mark on the apps pinned to Home. Holding an app pins or unpins it; the pins are kept in system.json (`thistle_os.launcher.pinned`) and survive reboots and updates.

**Blocked by:** 02.

**Status:** open

- [ ] Pages of 3 × 4 on a 240 × 320 screen, a page indicator, the keyboard's arrows or Tab between pages
- [ ] Pins read by `home_pick_pins()`: an uninstalled app's place goes to the next, as now
- [ ] Tiles focusable, as Home's are
