/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — a map on the screen
 *
 * A map view shows a Map Pack's tiles at one of the four levels, z14 to
 * z17, at any pixel offset. It moves as an e-paper map should: a step at a
 * time, each step half the view, and each step shown whole, once, so the
 * panel refreshes once a step and never shows a view half drawn.
 *
 * The tiles are read and inflated on the view's own task (`map_tiles`, an
 * 8 KB internal stack), never the UI task, and kept in a small cache in
 * PSRAM, least recently used out first. That task composes the next view
 * into a second buffer, also in PSRAM; the UI task swaps it in once it is
 * whole and still what was asked for. A view asked for while another is
 * being composed replaces it: a step asked for and overtaken by the next
 * is never drawn. The view's lv_image shows its buffer as it is, RGB565,
 * so LVGL's 64 KB pool holds none of it. Where the Pack has no tile, or a
 * damaged one, it shows the "no map here" hatching
 * (docs/maps/map-pack-format.md), aligned to the world's pixels.
 *
 * Input: a swipe across the view moves it a step the other way, as if the
 * map were dragged; the arrow keys, and w, a, s, d, move it a step; +, =
 * and i zoom in, - and o out, keeping the view's centre; a tap that is
 * not a swipe, and Enter, select the nearest overlay item
 * (map_overlay.h). Other keys are left for the app (LV_EVENT_KEY on
 * map_view_obj()).
 *
 * For built-in apps, on the UI task; the view does not own the Pack, which
 * must outlive it. The view makes the Pack's tile reads take a lock of
 * its own (map_pack_set_lock()), so the UI task may ask the Pack where it
 * has tiles while the view is up; nothing else may read it meanwhile.
 */
#pragma once

#include "map_engine/map_pack.h"
#include "map_engine/map_projection.h"
#include "lvgl.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct map_view map_view_t;

/* What a view now on the screen is, and what composing it took. */
typedef struct {
    int      zoom;
    int64_t  left, top;  /* the world pixel at the view's top left */
    int      tiles;      /* places shown from the Pack */
    int      missing;    /* places with no tile, hatched */
    int      damaged;    /* places whose tile failed its checks, hatched */
    int      read;       /* places read from the Pack for it */
    int      cached;     /* places found in the cache */
    uint32_t compose_ms; /* reading, inflating and composing */
} map_view_shown_t;

/* Called on the UI task each time a view is swapped onto the screen. */
typedef void (*map_view_shown_cb_t)(map_view_t *view, const map_view_shown_t *shown, void *user);

/* A view filling parent's content area, with its size as laid out now,
 * showing pack (which may be NULL: all hatching). Nothing is shown until
 * map_view_go_to(). NULL without the memory or the task. */
map_view_t *map_view_create(lv_obj_t *parent, map_pack_t *pack, map_view_shown_cb_t shown,
                            void *user);

/* Stops the view's task, waiting for a tile it is reading, and frees it. */
void map_view_destroy(map_view_t *view);

lv_obj_t *map_view_obj(map_view_t *view);
int map_view_width(const map_view_t *view);
int map_view_height(const map_view_t *view);

/* Shows (lat, lon) in the middle of the view, its pixel on the view's
 * middle pixel (x w/2, y h/2), at zoom, clamped to z14..z17. */
void map_view_go_to(map_view_t *view, double lat, double lon, int zoom);

/* Moves the view by steps of half its width (dx) and half its height (dy):
 * positive is east and south. */
void map_view_pan(map_view_t *view, int dx, int dy);

/* Zooms by levels (positive is in), keeping the world pixel at the view's
 * middle there. False, and nothing asked, when already at the end, or when
 * zooming in would show no tile of the Pack at all (its Regions there stop
 * at the level shown): that logs "no closer map here: ..." and calls the
 * no-closer callback. */
bool map_view_zoom_by(map_view_t *view, int levels);

/* Called on the UI task when a zoom in is refused for want of a closer
 * map, with the level shown, so the app can say so. */
typedef void (*map_view_no_closer_cb_t)(map_view_t *view, int zoom, void *user);
void map_view_set_no_closer_cb(map_view_t *view, map_view_no_closer_cb_t cb, void *user);

/* The app covers px at the bottom of the view (an About line, say): the
 * marker counts as off the view there, and near its edge above it. */
void map_view_set_bottom_inset(map_view_t *view, int px);

/* The device's position, from each fix: the view draws a marker there, a
 * ring with a dot. It is redrawn only once the fix has moved more than
 * 12 px on the screen, and at most every 5 s, so a refresh of its own
 * stays rare and small (only the marker's old and new places). A marker
 * the fix walks to within 24 px of the view's edge makes the map jump a
 * step, half the view, that way, again at most every 5 s; a marker the
 * user panned to the edge or away is left there. Logs what it does:
 * "marker moved 30 px to 150,148, 5.0 s after the last", "marker held:
 * ...", "page jump east to keep the marker in view: ...". */
void map_view_set_position(map_view_t *view, double lat, double lon);

/* Shows the position in the middle of the view at the zoom asked for
 * last. False, and nothing asked, without a position yet. */
bool map_view_locate(map_view_t *view);

/* The view asked for last, which may not be on the screen yet. */
int map_view_zoom(const map_view_t *view);
void map_view_origin(const map_view_t *view, int64_t *x, int64_t *y);

#ifdef __cplusplus
}
#endif
