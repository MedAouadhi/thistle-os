/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — a map on the screen
 *
 * A map view shows a Map Pack's tiles around a position at one of the four
 * levels, z14 to z17: the position in the middle of the view, the world's
 * pixel nearest it on the view's middle pixel (x w/2, y h/2). It composes
 * the view from up to six tiles at any pixel offset into one buffer in
 * PSRAM, which an lv_image shows as it is, in the display's own format, so
 * LVGL's 64 KB pool holds none of it. Where the Pack has no tile, or a
 * damaged one, the view shows the "no map here" hatching
 * (docs/maps/map-pack-format.md), aligned to the world's pixels.
 *
 * For built-in apps; the view does not own the Pack.
 */
#pragma once

#include "map_engine/map_pack.h"
#include "lvgl.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct map_view map_view_t;

/* What one composition drew. */
typedef struct {
    int tiles;    /* tiles of the Pack on the view */
    int missing;  /* places with no tile, hatched */
    int damaged;  /* tiles that failed their checks, hatched */
} map_view_stats_t;

/* A view filling parent's content area, with its size as laid out now. */
map_view_t *map_view_create(lv_obj_t *parent);
void map_view_destroy(map_view_t *view);

lv_obj_t *map_view_obj(map_view_t *view);
int map_view_width(const map_view_t *view);
int map_view_height(const map_view_t *view);

/* Shows pack (which may be NULL: all hatching) around (lat, lon) at zoom,
 * which is clamped to z14..z17. */
map_view_stats_t map_view_show(map_view_t *view, map_pack_t *pack, double lat, double lon,
                               int zoom);

/* The world pixel (Web Mercator, 256 px tiles at the shown zoom) at the
 * view's top left. */
void map_view_origin(const map_view_t *view, int64_t *x, int64_t *y);

/* Web Mercator: the world pixel of (lat, lon) at zoom. */
void map_world_pixel(double lat, double lon, int zoom, double *x, double *y);

/* Whether the "no map here" hatching inks world pixel (x, y). */
bool map_no_map_here(int64_t x, int64_t y);

#ifdef __cplusplus
}
#endif
