/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — composing a view from a Map Pack's tiles
 *
 * The part of the map view that needs neither LVGL nor a task: a small
 * cache of inflated tiles in PSRAM, least recently used first out, and the
 * composition of a view's pixels from them at any world offset. map_view.c
 * runs it on the view's tile task; the host tests run it as it is
 * (components/map_engine/tests/).
 *
 * Not thread-safe: one task uses a cache at a time.
 */
#pragma once

#include "map_engine/map_pack.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAP_INK   0x0000u  /* RGB565 */
#define MAP_PAPER 0xFFFFu

typedef struct map_tile_cache map_tile_cache_t;

/* What one composition drew, place by place: a place is a tile's square of
 * the world that the view touches. */
typedef struct {
    int tiles;    /* places shown from the Pack */
    int missing;  /* places with no tile, hatched */
    int damaged;  /* places whose tile failed its checks, hatched */
    int read;     /* places read from the Pack for this composition */
    int cached;   /* places found in the cache */
} map_compose_stats_t;

/* A cache of `slots` tiles, their pixels in one PSRAM block. NULL without
 * the memory. */
map_tile_cache_t *map_tile_cache_create(int slots);
void map_tile_cache_destroy(map_tile_cache_t *cache);

/* Composes the w x h view whose top left is world pixel (left, top) at
 * zoom into pixels (w * h RGB565, row by row): the Pack's tiles where it
 * has them, the "no map here" hatching elsewhere and where a tile is
 * damaged. pack may be NULL, which is all hatching. */
void map_compose(map_tile_cache_t *cache, map_pack_t *pack, int zoom, int64_t left, int64_t top,
                 int w, int h, uint16_t *pixels, map_compose_stats_t *stats);

/* Whether the Pack holds any tile the w x h view whose top left is world
 * pixel (left, top) at zoom touches: a view with none is all hatching.
 * Reads only the index. False for a NULL pack or a zoom outside z14..z17. */
bool map_compose_holds_tiles(map_pack_t *pack, int zoom, int64_t left, int64_t top, int w, int h);

#ifdef __cplusplus
}
#endif
