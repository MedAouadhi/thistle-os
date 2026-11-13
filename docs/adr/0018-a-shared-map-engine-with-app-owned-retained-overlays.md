# A shared map engine, with overlays each app owns and retains

The Maps app shows a Map Pack, and other built-in apps, a Berlin transit app
first, want their own data on a map: stops, lines, vehicles that move. They
should not each write a map, and nothing they draw may make the e-paper
panel flash or the device run short of internal RAM.

**The map is a library component, `components/map_engine/`**, with no app of
its own. An app makes a view (`map_view.h`) and puts **Overlays** on it
(`map_overlay.h`, `docs/maps/overlay-api.md`): points with icons and labels,
lines, polygons and text, in latitude and longitude.

**An overlay belongs to the app that made it.** There is no kernel-wide
registry and no syscall: apps share data by writing GeoJSON files to
`/sdcard/maps/overlays/`, which the Maps app shows as switchable overlays.

**Overlays are retained, not drawn through a callback.** The app sets the
items and says it has changed them; the engine draws them over the tiles in
z order, decluttered, with one-bit primitives, whenever it shows a view. So
it decides when a change reaches the panel: at most every 20 s, only while
the view is on the screen, and then only the rectangle of the screen that
changed, which it finds by comparing the screen drawn with the one before.
A Refresh draws at once. An overlay holds at most 256 items, so a large feed
can flood neither the panel nor the memory.

**Labels go through LVGL's own text drawing**, into the view's pixels, so
Arabic is shaped and joined by the same wraps as every label, then made one
bit a pixel again. The engine has no font or shaper of its own.

## Considered Options

**A per-frame draw callback**, as many map widgets offer, would leave each
app to decide when to draw, and so when the panel refreshes; one careless
app would flash it every second. It would also run app code in the middle of
composing a view.

**One LVGL object per item** would need no drawing code, but at about
0.75 KB of LVGL's 64 KB pool an object, 256 items would not fit.

**A kernel-wide overlay registry**, which any app could add to, would let
the Maps app show another app's live data directly, at the cost of
lifetimes across apps, permissions, and data kept while its app is closed.
GeoJSON files do the sharing with none of that.

## Consequences

An app's change can wait up to 20 s to show unless it offers Refresh. The
view keeps two more screen-sized buffers in PSRAM. Packaged apps cannot put
anything on a map until syscalls for it are designed. Declutter is greedy,
in order, so an app orders its items by importance.
