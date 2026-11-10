/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — things an app draws over a map
 *
 * An overlay is an app's own set of items in latitude and longitude, kept
 * by the app and shown by any map view it is added to (map_view.h): the
 * view projects, clips and draws them over the tiles, one bit a pixel, as
 * it composes each view on its tile task, so a step with an overlay on
 * still costs one refresh. Overlays are what can be switched on and off;
 * a Pack's tiles never hold anything switchable.
 *
 * Lines only, so far: each drawn as a black line 3 px wide in a paper
 * casing 7 px wide, which keeps it apart from the map's own ink. An
 * overlay holds at most MAP_OVERLAY_MAX_ITEMS lines and
 * MAP_OVERLAY_MAX_POINTS points in all; past either, what is added is
 * refused. Its points are in PSRAM.
 *
 * Built on the UI task, then added to a view; an overlay added to a view
 * is read by the view's task and must not change until it is removed or
 * the view destroyed.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "map_engine/map_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAP_OVERLAY_MAX_ITEMS  256
#define MAP_OVERLAY_MAX_POINTS 16384
#define MAP_OVERLAY_NAME_MAX   24

typedef struct map_overlay map_overlay_t;

/* An empty overlay named name (no spaces: it is in the log). NULL without
 * the memory. */
map_overlay_t *map_overlay_create(const char *name);
void map_overlay_destroy(map_overlay_t *overlay);

const char *map_overlay_name(const map_overlay_t *overlay);

/* Starts a new line. False when the overlay holds its most lines. */
bool map_overlay_line_begin(map_overlay_t *overlay);

/* Adds a point to the line begun last. False with no line begun, or when
 * the overlay holds its most points. */
bool map_overlay_line_point(map_overlay_t *overlay, double lat, double lon);

int map_overlay_item_count(const map_overlay_t *overlay);
int map_overlay_point_count(const map_overlay_t *overlay);

/* Draws the overlay over a w x h view whose top left is world pixel
 * (left, top) at zoom, into pixels (RGB565, row by row). */
void map_overlay_draw(const map_overlay_t *overlay, int zoom, int64_t left, int64_t top, int w,
                      int h, uint16_t *pixels);

#ifdef __cplusplus
}
#endif
