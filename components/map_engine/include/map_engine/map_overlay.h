/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — an app's own things on a map
 *
 * An Overlay is a set of items an app puts on a map view: points with an
 * icon and an optional label, lines, polygons and text, at latitudes and
 * longitudes. The app owns it (no other app sees it; apps share by writing
 * GeoJSON files, which the Maps app shows), and it is retained: the app
 * sets the items once, the view draws them over the tiles each time it is
 * shown, and the app calls map_overlay_changed() when it has changed them.
 * docs/maps/overlay-api.md has a worked example.
 *
 * How it is drawn, on e-paper first, one bit a pixel:
 *
 *   - Overlays in their z order, the highest on top; in one overlay its
 *     polygons (outlined, dotted inside), then its lines, then its points'
 *     icons, then their labels and the texts. Lines and outlines are 3 px
 *     of ink inside 5 px of paper, so they show on any map.
 *   - Decluttered: a point or a text whose box would cover another one
 *     already placed is left out, and a label that would is dropped while
 *     its icon stays. Higher overlays are placed first and, in an overlay,
 *     earlier items: add the important ones first. Lines and polygons are
 *     never left out.
 *   - Labels and texts are in the system font, Arabic shaped and joined as
 *     every label is (CLAUDE.md, Arabic text), on a paper box, one line of
 *     at most 120 px.
 *
 * When it is drawn: a change shows at most every 20 s (MAP_OVERLAY_LIVE_MS
 * after the overlays were last drawn), and only while the view is on the
 * screen; then only the rectangle of the screen that changed is flushed,
 * so the e-paper panel refreshes only that. map_view_refresh() draws a
 * change at once, for an app's Refresh. Switching an overlay on or off, a
 * pan or a zoom draws at once too.
 *
 * Selection: a tap on the view that is not a swipe, or Enter (the view's
 * middle), finds the nearest item drawn within MAP_OVERLAY_HIT_PX, over
 * every overlay switched on, and calls that overlay's tap callback. A tap
 * that finds nothing calls the view's miss callback.
 *
 * All of it on the UI task, as the view. An overlay must be destroyed
 * before its view.
 */
#pragma once

#include "map_engine/map_view.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAP_OVERLAY_MAX_ITEMS    256     /* items an overlay holds */
#define MAP_OVERLAY_MAX_VERTICES 16384   /* of all its lines and polygons */
#define MAP_OVERLAY_LABEL_MAX    64      /* bytes of a label or text, cut to fit */
#define MAP_OVERLAY_NAME_MAX     32
#define MAP_OVERLAY_HIT_PX       16      /* how near a tap must be */
#define MAP_OVERLAY_LIVE_MS      20000   /* a change shows at most this often */

typedef struct map_overlay map_overlay_t;

typedef struct {
    double lat, lon;
} map_lat_lon_t;

typedef enum {
    MAP_ICON_DOT = 0,  /* a filled disc */
    MAP_ICON_CIRCLE,   /* a ring */
    MAP_ICON_SQUARE,
    MAP_ICON_TRIANGLE,
    MAP_ICON_CROSS,
    MAP_ICON_COUNT,
} map_icon_t;

typedef enum {
    MAP_ITEM_POINT = 0,
    MAP_ITEM_LINE,
    MAP_ITEM_POLYGON,
    MAP_ITEM_TEXT,
} map_item_kind_t;

/* An item of `overlay` was tapped: `item` is what its add call returned,
 * `id` what the app gave it. */
typedef void (*map_overlay_tap_cb_t)(map_overlay_t *overlay, int item, uint32_t id, void *user);

/* A tap on the view found no item within MAP_OVERLAY_HIT_PX; x, y are in
 * the view. */
typedef void (*map_view_miss_cb_t)(map_view_t *view, int x, int y, void *user);

/* An empty overlay on view, switched on, at z (higher is on top; equal z
 * goes in the order made). tap may be NULL. NULL without the memory. */
map_overlay_t *map_overlay_create(map_view_t *view, const char *name, int z,
                                  map_overlay_tap_cb_t tap, void *user);

/* Takes it off its view, drawn at once, and frees it. NULL is nothing. */
void map_overlay_destroy(map_overlay_t *overlay);

/* Adding returns the item's index, from 0 in the order added, or -1 when
 * the overlay is full (MAP_OVERLAY_MAX_ITEMS items, or the vertices) or a
 * line has fewer than 2 points, a polygon fewer than 3. label may be NULL.
 * Nothing shows until map_overlay_changed(). */
int map_overlay_add_point(map_overlay_t *overlay, double lat, double lon, map_icon_t icon,
                          const char *label, uint32_t id);
int map_overlay_add_text(map_overlay_t *overlay, double lat, double lon, const char *text,
                         uint32_t id);
int map_overlay_add_line(map_overlay_t *overlay, const map_lat_lon_t *points, int count,
                         uint32_t id);
/* A ring: the last point joins the first, which need not be repeated. */
int map_overlay_add_polygon(map_overlay_t *overlay, const map_lat_lon_t *points, int count,
                            uint32_t id);

/* Removes every item; nothing shows until map_overlay_changed(). */
void map_overlay_clear(map_overlay_t *overlay);

/* The app is done changing it: it is drawn as said above. */
void map_overlay_changed(map_overlay_t *overlay);

/* Switches it on or off, drawn at once. */
void map_overlay_set_visible(map_overlay_t *overlay, bool visible);
bool map_overlay_visible(const map_overlay_t *overlay);

int map_overlay_count(const map_overlay_t *overlay);
const char *map_overlay_name(const map_overlay_t *overlay);
map_item_kind_t map_overlay_item_kind(const map_overlay_t *overlay, int item);
/* An item's label or text, "" for none. */
const char *map_overlay_item_label(const map_overlay_t *overlay, int item);

/* Draws every change now, whenever the last draw was: an app's Refresh. */
void map_view_refresh(map_view_t *view);

/* Called for a tap that selects nothing; NULL for none. */
void map_view_set_miss_cb(map_view_t *view, map_view_miss_cb_t miss, void *user);

/* The screen pixel of (lat, lon) in the view now asked for, which may be
 * outside it; and back. For an app that places things itself. */
void map_view_to_screen(const map_view_t *view, double lat, double lon, int *x, int *y);
void map_view_from_screen(const map_view_t *view, int x, int y, double *lat, double *lon);

#ifdef __cplusplus
}
#endif
