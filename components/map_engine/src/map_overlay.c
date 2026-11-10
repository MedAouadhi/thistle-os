/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — things an app draws over a map (map_overlay.h)
 *
 * A point is kept as its world pixel at z17, the deepest level, so drawing
 * at any level only scales it: Web Mercator doubles a level. A line is
 * drawn segment by segment, each pixel whose centre is near enough the
 * segment inked: first every segment's paper casing, then every segment's
 * ink, so a line crossing itself keeps its ink.
 */
#include "map_engine/map_overlay.h"
#include "map_engine/map_projection.h"
#include "map_compose.h"

#include "esp_heap_caps.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define INK_RADIUS    1.5   /* px: a line 3 px wide */
#define CASING_RADIUS 3.5   /* px: in paper 7 px wide */

typedef struct {
    double x, y;  /* the world pixel at MAP_MAX_ZOOM */
} point_t;

struct map_overlay {
    char     name[MAP_OVERLAY_NAME_MAX];
    point_t *points;                         /* MAP_OVERLAY_MAX_POINTS, PSRAM */
    int      point_count;
    int      line_start[MAP_OVERLAY_MAX_ITEMS];  /* each line's first point */
    int      line_count;
};

static void *alloc_psram(size_t len)
{
    void *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(len);
}

map_overlay_t *map_overlay_create(const char *name)
{
    map_overlay_t *o = alloc_psram(sizeof *o);
    if (!o) return NULL;
    memset(o, 0, sizeof *o);
    o->points = alloc_psram(sizeof(point_t) * MAP_OVERLAY_MAX_POINTS);
    if (!o->points) {
        free(o);
        return NULL;
    }
    strncpy(o->name, name ? name : "overlay", sizeof o->name - 1);
    return o;
}

void map_overlay_destroy(map_overlay_t *o)
{
    if (!o) return;
    free(o->points);
    free(o);
}

const char *map_overlay_name(const map_overlay_t *o) { return o->name; }
int map_overlay_item_count(const map_overlay_t *o) { return o->line_count; }
int map_overlay_point_count(const map_overlay_t *o) { return o->point_count; }

bool map_overlay_line_begin(map_overlay_t *o)
{
    if (o->line_count >= MAP_OVERLAY_MAX_ITEMS) return false;
    /* A line begun and left empty is begun again. */
    if (o->line_count > 0 && o->line_start[o->line_count - 1] == o->point_count) return true;
    o->line_start[o->line_count++] = o->point_count;
    return true;
}

bool map_overlay_line_point(map_overlay_t *o, double lat, double lon)
{
    if (o->line_count == 0 || o->point_count >= MAP_OVERLAY_MAX_POINTS) return false;
    if (!isfinite(lat) || !isfinite(lon) || lat < -85.06 || lat > 85.06 || lon < -180.0 ||
        lon > 180.0) {
        return false;
    }
    point_t *p = &o->points[o->point_count++];
    map_world_pixel(lat, lon, MAP_MAX_ZOOM, &p->x, &p->y);
    return true;
}

/* Sets each pixel of the view whose centre is within radius of segment
 * (ax, ay)-(bx, by), in the view's pixels, to colour. */
static void draw_segment(double ax, double ay, double bx, double by, double radius,
                         uint16_t colour, int w, int h, uint16_t *pixels)
{
    double x0 = fmin(ax, bx) - radius, x1 = fmax(ax, bx) + radius;
    double y0 = fmin(ay, by) - radius, y1 = fmax(ay, by) + radius;
    if (x1 < 0 || y1 < 0 || x0 >= w || y0 >= h) return;
    int sx0 = x0 < 0 ? 0 : (int)x0, sx1 = x1 >= w ? w - 1 : (int)x1;
    int sy0 = y0 < 0 ? 0 : (int)y0, sy1 = y1 >= h ? h - 1 : (int)y1;
    double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    double r2 = radius * radius;
    for (int y = sy0; y <= sy1; y++) {
        double py = y + 0.5;
        for (int x = sx0; x <= sx1; x++) {
            double px = x + 0.5;
            double t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            double ex = px - (ax + t * dx), ey = py - (ay + t * dy);
            if (ex * ex + ey * ey <= r2) pixels[(size_t)y * w + x] = colour;
        }
    }
}

static void draw_lines(const map_overlay_t *o, double scale, int64_t left, int64_t top,
                       double radius, uint16_t colour, int w, int h, uint16_t *pixels)
{
    for (int l = 0; l < o->line_count; l++) {
        int first = o->line_start[l];
        int end = l + 1 < o->line_count ? o->line_start[l + 1] : o->point_count;
        for (int i = first; i < end; i++) {
            const point_t *a = &o->points[i];
            const point_t *b = i + 1 < end ? &o->points[i + 1] : a;
            if (i + 1 == end && end - first > 1) break;  /* a lone point only */
            draw_segment(a->x * scale - (double)left, a->y * scale - (double)top,
                         b->x * scale - (double)left, b->y * scale - (double)top, radius, colour,
                         w, h, pixels);
        }
    }
}

void map_overlay_draw(const map_overlay_t *o, int zoom, int64_t left, int64_t top, int w, int h,
                      uint16_t *pixels)
{
    if (!o || !pixels || w <= 0 || h <= 0) return;
    double scale = ldexp(1.0, zoom - MAP_MAX_ZOOM);
    draw_lines(o, scale, left, top, CASING_RADIUS, MAP_PAPER, w, h, pixels);
    draw_lines(o, scale, left, top, INK_RADIUS, MAP_INK, w, h, pixels);
}
