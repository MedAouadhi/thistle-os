/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — where things are on a map
 *
 * Web Mercator with 256-pixel tiles, as docs/maps/map-pack-format.md has
 * it: at zoom z the world is 256 x 2^z pixels a side.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The world pixel of (lat, lon) at zoom. */
void map_world_pixel(double lat, double lon, int zoom, double *x, double *y);

/* Whether the "no map here" hatching inks world pixel (x, y). */
bool map_no_map_here(int64_t x, int64_t y);

#ifdef __cplusplus
}
#endif
