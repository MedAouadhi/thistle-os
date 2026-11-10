# Putting things on a map: the overlay C API

A built-in app that embeds a map (`components/map_engine/`) can draw its own
things over it: points with icons and labels, lines, polygons and text. This
is the API for that; the headers, `map_engine/map_view.h` and
`map_engine/map_overlay.h`, are the reference. ADR-0018 has the reasons.
Packaged apps cannot use it: there are no syscalls for it yet.

Everything here runs on the UI task (LVGL's), as the view does.

## The shape of it

- **An Overlay belongs to the app that made it**, on a view the app made.
  There is no registry: another app does not see it. To share data, write a
  GeoJSON file to `/sdcard/maps/overlays/`, which the Maps app shows (below).
- **It is retained.** The app adds items and calls `map_overlay_changed()`;
  the view draws them over the tiles every time it shows a view, a pan or a
  zoom included. There is no per-frame callback.
- **At most 256 items an overlay** (`MAP_OVERLAY_MAX_ITEMS`), and 16384
  vertices across its lines and polygons. An add past either returns -1.
- **Decluttered.** A point or a text whose box would cover one already placed
  is left out; a label that would is dropped and its icon kept. Higher
  overlays are placed first and, within one, earlier items: add the
  important ones first. Lines and polygons are never left out.
- **Live, but calm.** A change is drawn at most every 20 s
  (`MAP_OVERLAY_LIVE_MS` after the overlays were last drawn), only while the
  view is on the screen, and only the rectangle that changed is flushed, so
  the e-paper panel refreshes only that. `map_view_refresh()` draws a change
  at once, for a Refresh key or button. Switching an overlay on or off, a
  pan and a zoom draw at once too.
- **Selection.** A tap on the view that is not a swipe, or Enter (at the
  view's middle), finds the nearest item drawn within 16 px
  (`MAP_OVERLAY_HIT_PX`) over every overlay switched on and calls its
  overlay's tap callback. A tap that finds nothing calls the view's miss
  callback, if it has one.

## How items look

One bit a pixel, made for e-paper:

| Item | Drawn as |
|------|----------|
| Point | an icon in a 15 px paper halo: `MAP_ICON_DOT`, `_CIRCLE`, `_SQUARE`, `_TRIANGLE`, `_CROSS`; its label to the right |
| Line | 3 px of ink inside 7 px of paper, so it shows on any street |
| Polygon | its outline as a line, its inside dotted every 4 px |
| Text | its text alone, centred on the place |

Overlays are drawn lowest z first. Within one: polygons, lines, then icons;
then every label and text, in Montserrat 14 on a paper box, one line of at
most 120 px. Arabic is shaped and joined, as in every LVGL label
(CLAUDE.md, "Arabic text"). A label or text is cut at 63 bytes, at a
character's end.

## An example

A Transit app showing stops, and the vehicles it fetches every 30 s:

```c
#include "map_engine/map_overlay.h"

static map_view_t    *s_view;
static map_overlay_t *s_stops, *s_vehicles;

static void stop_tapped(map_overlay_t *ov, int item, uint32_t id, void *user)
{
    /* id is what the app gave the item: here, its stop number. */
    show_departures(id);
}

void transit_ui_create(lv_obj_t *parent, map_pack_t *pack)
{
    s_view = map_view_create(parent, pack, NULL, NULL);
    map_view_go_to(s_view, 52.5219, 13.4132, 16);

    s_stops = map_overlay_create(s_view, "stops", 1, stop_tapped, NULL);
    for (int i = 0; i < stop_count; i++) {
        map_overlay_add_point(s_stops, stops[i].lat, stops[i].lon, MAP_ICON_SQUARE,
                              stops[i].name, stops[i].number);
    }
    map_overlay_changed(s_stops);

    s_vehicles = map_overlay_create(s_view, "vehicles", 2, NULL, NULL);
}

/* On the UI task, from an lv_timer that finds the Provider's task has left
 * something new. */
void transit_ui_vehicles(const vehicle_t *v, int n)
{
    map_overlay_clear(s_vehicles);
    for (int i = 0; i < n; i++) {
        map_overlay_add_point(s_vehicles, v[i].lat, v[i].lon, MAP_ICON_TRIANGLE, v[i].line, i);
    }
    map_overlay_changed(s_vehicles);  /* drawn now, or 20 s after the last draw */
}

void transit_ui_refresh_pressed(void)
{
    map_view_refresh(s_view);         /* drawn now */
}

void transit_ui_destroy(void)
{
    map_overlay_destroy(s_vehicles);  /* every overlay before its view */
    map_overlay_destroy(s_stops);
    map_view_destroy(s_view);
}
```

A line or a polygon takes an array of `map_lat_lon_t`; a polygon's last
point joins its first, which need not be repeated:

```c
map_lat_lon_t route[] = {{52.52, 13.40}, {52.53, 13.41}, {52.53, 13.43}};
map_overlay_add_line(s_lines, route, 3, 7);
```

`map_view_to_screen()` and `map_view_from_screen()` turn latitude and
longitude into a pixel of the view and back, for an app that places a
widget of its own over the map.

## The calls

| Call | What it does |
|------|--------------|
| `map_overlay_create(view, name, z, tap, user)` | an empty overlay, switched on; higher z on top |
| `map_overlay_destroy(o)` | takes it off, drawn at once, and frees it; before the view |
| `map_overlay_add_point(o, lat, lon, icon, label, id)` | a point; label may be NULL |
| `map_overlay_add_text(o, lat, lon, text, id)` | a text with no icon |
| `map_overlay_add_line(o, points, n, id)` | a line of n >= 2 points |
| `map_overlay_add_polygon(o, points, n, id)` | a polygon of n >= 3 points |
| `map_overlay_clear(o)` | removes every item |
| `map_overlay_changed(o)` | done changing it: drawn under the 20 s rule |
| `map_overlay_set_visible(o, on)`, `map_overlay_visible(o)` | on or off, drawn at once |
| `map_overlay_count`, `_name`, `_item_kind`, `_item_label` | what it holds |
| `map_view_refresh(view)` | draws every change now |
| `map_view_set_miss_cb(view, cb, user)` | called for a tap that selects nothing |
| `map_view_to_screen`, `map_view_from_screen` | latitude and longitude to a pixel of the view, and back |

Adds return the item's index, from 0 in the order added, which the tap
callback gets back with the item's `id`. Items added show only once
`map_overlay_changed()` is called, so an app can clear and refill an overlay
with no half-drawn state.

The view logs what it draws, which a simulator test can read:

    (map_view) overlays drawn: 2 on, 40 items in view, 12 left out to declutter
    (map_view) overlay "vehicles" changed; drawn in 13.9 s, 20 s after the last
    (map_view) overlays redrawn for a change, 20.0 s after the last: changed 100,140-130,160
    (map_view) tap at 120,172: "stops" item 3, 4 px away

## Memory

An overlay's items, labels and vertices are in PSRAM, about 26 KB plus 8
bytes a vertex. The view keeps two more screen-sized buffers in PSRAM for
its overlays, 142 KB each on the T-Deck Max, for the screen as shown and the
next one, which it compares to find what changed. Drawing runs on the UI
task and allocates nothing in internal RAM but a few hundred bytes for a
polygon's crossings.

## GeoJSON files in the Maps app

Every `*.geojson` and `*.json` in `/sdcard/maps/overlays/`, up to eight, by
name, is an overlay in the Maps app, named by its file without the suffix,
switched on and off in its Layers panel. A file is a FeatureCollection:

- `Point` and `MultiPoint`: points, labelled with `properties.name` (or
  `title`), the icon named by `marker-symbol` (or `icon`): `circle`,
  `square`, `triangle`, `cross` (or `hospital`), else a dot; `text` for a
  label with no icon.
- `LineString`, `MultiLineString`: lines.
- `Polygon`, `MultiPolygon`: their outer rings; holes are not drawn.

The rest of a file past 256 items is left out and logged (`(maps) overlay
X.geojson: 256 items, 44 left out (256 at most)`). A file that changes on the
card, as a Provider saving its last result does, is read again within 2 s
and drawn under the 20 s rule; Refresh (the button, or `r`) draws it at once.
A tap near an item opens its info card: its name, and each of its other
properties that is a string, a number or a boolean, ten at most. A tap on
the card, or on nothing, closes it. A file is read whole into PSRAM, 1 MB at
most.
