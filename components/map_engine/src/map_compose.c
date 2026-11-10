/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — composing a view from a Map Pack's tiles
 *
 * map_compose.h says what it does. The cache holds a tile's place whatever
 * the Pack answered, so a place outside every Region or a damaged tile is
 * not read from the card again either; it is emptied only with the view.
 */
#include "map_compose.h"
#include "map_engine/map_projection.h"

#include "esp_heap_caps.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int               zoom;  /* 0: empty */
    int64_t           x, y;
    map_tile_result_t result;
    uint32_t          used;  /* when last used, by the cache's clock */
} slot_t;

struct map_tile_cache {
    int      count;
    uint32_t clock;
    slot_t  *slots;
    uint8_t *bits;  /* count * MAP_TILE_BYTES, in PSRAM */
};

static void *alloc_psram(size_t len)
{
    void *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(len);
}

void map_world_pixel(double lat, double lon, int zoom, double *x, double *y)
{
    const double size = 256.0 * (double)(1u << zoom);
    if (lat > 85.05112878) lat = 85.05112878;
    if (lat < -85.05112878) lat = -85.05112878;
    double s = sin(lat * M_PI / 180.0);
    *x = (lon + 180.0) / 360.0 * size;
    *y = (0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI)) * size;
}

bool map_no_map_here(int64_t x, int64_t y)
{
    return (x + y) % 8 == 0 || (x - y) % 8 == 0;
}

map_tile_cache_t *map_tile_cache_create(int slots)
{
    if (slots < 1) return NULL;
    map_tile_cache_t *cache = calloc(1, sizeof *cache);
    if (!cache) return NULL;
    cache->count = slots;
    cache->slots = calloc((size_t)slots, sizeof *cache->slots);
    cache->bits = alloc_psram((size_t)slots * MAP_TILE_BYTES);
    if (!cache->slots || !cache->bits) {
        map_tile_cache_destroy(cache);
        return NULL;
    }
    return cache;
}

void map_tile_cache_destroy(map_tile_cache_t *cache)
{
    if (!cache) return;
    free(cache->slots);
    free(cache->bits);  /* heap_caps memory is free()d too */
    free(cache);
}

/* The slot holding place (zoom, x, y), read from the Pack if it is not
 * cached; NULL never. */
static slot_t *place(map_tile_cache_t *cache, map_pack_t *pack, int zoom, int64_t x, int64_t y,
                     map_compose_stats_t *stats)
{
    slot_t *oldest = &cache->slots[0];
    for (int i = 0; i < cache->count; i++) {
        slot_t *s = &cache->slots[i];
        if (s->zoom == zoom && s->x == x && s->y == y) {
            s->used = ++cache->clock;
            stats->cached++;
            return s;
        }
        if (s->zoom == 0 || (oldest->zoom != 0 && s->used < oldest->used)) oldest = s;
    }
    slot_t *s = oldest;
    uint8_t *bits = cache->bits + (size_t)(s - cache->slots) * MAP_TILE_BYTES;
    const int64_t world_tiles = (int64_t)1 << zoom;
    s->zoom = zoom;
    s->x = x;
    s->y = y;
    s->used = ++cache->clock;
    s->result = MAP_TILE_NONE;
    if (pack && x >= 0 && y >= 0 && x < world_tiles && y < world_tiles) {
        s->result = map_pack_read_tile(pack, zoom, (uint32_t)x, (uint32_t)y, bits);
    }
    stats->read++;
    return s;
}

/* Fills the view's part of world tile (tx, ty): from the tile's bits when
 * there are some, else hatching. */
static void fill_part(uint16_t *pixels, int64_t left, int64_t top, int w, int h,
                      const uint8_t *bits, int64_t tx, int64_t ty)
{
    int64_t x0 = tx * MAP_TILE_PX, y0 = ty * MAP_TILE_PX;
    int64_t from_x = x0 > left ? x0 : left;
    int64_t from_y = y0 > top ? y0 : top;
    int64_t to_x = x0 + MAP_TILE_PX < left + w ? x0 + MAP_TILE_PX : left + w;
    int64_t to_y = y0 + MAP_TILE_PX < top + h ? y0 + MAP_TILE_PX : top + h;
    for (int64_t gy = from_y; gy < to_y; gy++) {
        uint16_t *row = pixels + (size_t)(gy - top) * (size_t)w;
        const uint8_t *src = bits ? bits + (size_t)(gy - y0) * (MAP_TILE_PX / 8) : NULL;
        for (int64_t gx = from_x; gx < to_x; gx++) {
            bool ink;
            if (src) {
                int px = (int)(gx - x0);
                ink = (src[px >> 3] & (0x80 >> (px & 7))) != 0;
            } else {
                ink = map_no_map_here(gx, gy);
            }
            row[gx - left] = ink ? MAP_INK : MAP_PAPER;
        }
    }
}

/* The tile holding world pixel v: floor(v / 256), for negative v too. */
static int64_t tile_of(int64_t v)
{
    return v >= 0 ? v / MAP_TILE_PX : -((-v + MAP_TILE_PX - 1) / MAP_TILE_PX);
}

void map_compose(map_tile_cache_t *cache, map_pack_t *pack, int zoom, int64_t left, int64_t top,
                 int w, int h, uint16_t *pixels, map_compose_stats_t *stats)
{
    memset(stats, 0, sizeof *stats);
    if (zoom < MAP_MIN_ZOOM) zoom = MAP_MIN_ZOOM;
    if (zoom > MAP_MAX_ZOOM) zoom = MAP_MAX_ZOOM;
    for (int64_t ty = tile_of(top); ty <= tile_of(top + h - 1); ty++) {
        for (int64_t tx = tile_of(left); tx <= tile_of(left + w - 1); tx++) {
            slot_t *s = place(cache, pack, zoom, tx, ty, stats);
            if (s->result == MAP_TILE_OK) stats->tiles++;
            else if (s->result == MAP_TILE_BAD) stats->damaged++;
            else stats->missing++;
            const uint8_t *bits = s->result == MAP_TILE_OK
                                      ? cache->bits + (size_t)(s - cache->slots) * MAP_TILE_BYTES
                                      : NULL;
            fill_part(pixels, left, top, w, h, bits, tx, ty);
        }
    }
}
