/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — a map on the screen
 *
 * map_view.h says what it shows. The tiles are read and inflated here, on
 * the caller's task; moving that off the UI task, with a small cache of
 * tiles in PSRAM, is the pan and zoom work's (ticket 02 of .scratch/maps).
 */
#include "map_engine/map_view.h"

#include "esp_heap_caps.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define INK   0x0000u  /* RGB565 */
#define PAPER 0xFFFFu

struct map_view {
    lv_obj_t      *image;
    lv_image_dsc_t dsc;
    uint16_t      *pixels;  /* w * h, in PSRAM */
    uint8_t       *tile;    /* MAP_TILE_BYTES, in PSRAM */
    int            w, h;
    int64_t        left, top;
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

map_view_t *map_view_create(lv_obj_t *parent)
{
    lv_obj_update_layout(parent);
    int w = lv_obj_get_content_width(parent);
    int h = lv_obj_get_content_height(parent);
    if (w <= 0 || h <= 0) return NULL;

    map_view_t *view = calloc(1, sizeof *view);
    if (!view) return NULL;
    view->w = w;
    view->h = h;
    view->pixels = alloc_psram((size_t)w * h * sizeof(uint16_t));
    view->tile = alloc_psram(MAP_TILE_BYTES);
    if (!view->pixels || !view->tile) {
        map_view_destroy(view);
        return NULL;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) view->pixels[i] = PAPER;

    view->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    view->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    view->dsc.header.w = (uint32_t)w;
    view->dsc.header.h = (uint32_t)h;
    view->dsc.header.stride = (uint32_t)w * sizeof(uint16_t);
    view->dsc.data_size = (uint32_t)((size_t)w * h * sizeof(uint16_t));
    view->dsc.data = (const uint8_t *)view->pixels;

    view->image = lv_image_create(parent);
    lv_obj_remove_flag(view->image, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(view->image, 0, 0);
    lv_obj_set_size(view->image, w, h);
    lv_image_set_src(view->image, &view->dsc);
    return view;
}

void map_view_destroy(map_view_t *view)
{
    if (!view) return;
    if (view->image) {
        lv_image_cache_drop(&view->dsc);
        lv_obj_delete(view->image);
    }
    free(view->pixels);
    free(view->tile);
    free(view);
}

lv_obj_t *map_view_obj(map_view_t *view) { return view->image; }
int map_view_width(const map_view_t *view) { return view->w; }
int map_view_height(const map_view_t *view) { return view->h; }

void map_view_origin(const map_view_t *view, int64_t *x, int64_t *y)
{
    *x = view->left;
    *y = view->top;
}

/* Fills the view's part of world tile (tx, ty): from the tile when there
 * is one, else hatching. */
static void fill_part(map_view_t *view, const uint8_t *bits, int64_t tx, int64_t ty)
{
    int64_t x0 = tx * MAP_TILE_PX, y0 = ty * MAP_TILE_PX;
    int64_t from_x = x0 > view->left ? x0 : view->left;
    int64_t from_y = y0 > view->top ? y0 : view->top;
    int64_t to_x = x0 + MAP_TILE_PX < view->left + view->w ? x0 + MAP_TILE_PX : view->left + view->w;
    int64_t to_y = y0 + MAP_TILE_PX < view->top + view->h ? y0 + MAP_TILE_PX : view->top + view->h;
    for (int64_t gy = from_y; gy < to_y; gy++) {
        uint16_t *row = view->pixels + (size_t)(gy - view->top) * view->w;
        const uint8_t *src = bits ? bits + (size_t)(gy - y0) * (MAP_TILE_PX / 8) : NULL;
        for (int64_t gx = from_x; gx < to_x; gx++) {
            bool ink;
            if (src) {
                int px = (int)(gx - x0);
                ink = (src[px >> 3] & (0x80 >> (px & 7))) != 0;
            } else {
                ink = map_no_map_here(gx, gy);
            }
            row[gx - view->left] = ink ? INK : PAPER;
        }
    }
}

map_view_stats_t map_view_show(map_view_t *view, map_pack_t *pack, double lat, double lon,
                               int zoom)
{
    map_view_stats_t stats = {0};
    if (zoom < MAP_MIN_ZOOM) zoom = MAP_MIN_ZOOM;
    if (zoom > MAP_MAX_ZOOM) zoom = MAP_MAX_ZOOM;

    double gx, gy;
    map_world_pixel(lat, lon, zoom, &gx, &gy);
    view->left = (int64_t)floor(gx + 0.5) - view->w / 2;
    view->top = (int64_t)floor(gy + 0.5) - view->h / 2;

    const int64_t world_tiles = (int64_t)1 << zoom;
    for (int64_t ty = view->top >> 8; ty <= (view->top + view->h - 1) >> 8; ty++) {
        for (int64_t tx = view->left >> 8; tx <= (view->left + view->w - 1) >> 8; tx++) {
            map_tile_result_t got = MAP_TILE_NONE;
            if (pack && tx >= 0 && ty >= 0 && tx < world_tiles && ty < world_tiles) {
                got = map_pack_read_tile(pack, zoom, (uint32_t)tx, (uint32_t)ty, view->tile);
            }
            if (got == MAP_TILE_OK) stats.tiles++;
            else if (got == MAP_TILE_BAD) stats.damaged++;
            else stats.missing++;
            fill_part(view, got == MAP_TILE_OK ? view->tile : NULL, tx, ty);
        }
    }

    lv_image_cache_drop(&view->dsc);
    lv_obj_invalidate(view->image);
    return stats;
}
