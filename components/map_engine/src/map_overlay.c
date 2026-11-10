/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — an app's own things on a map
 *
 * map_overlay.h says what an overlay is and how it is drawn. Here: the
 * items, kept in PSRAM; their drawing, one bit a pixel, into the view's
 * RGB565 pixels; the declutter; and the hit test.
 *
 * Positions are kept as world pixels at z17, fractions and all, so drawing
 * at any of the four levels is a scale, with no trigonometry per frame.
 * Lines and outlines are drawn as round pens, each pixel whose centre is
 * near enough the segment inked: an overlay's line falls where a track's
 * points put it, to the fraction of a pixel.
 *
 * Labels are drawn by LVGL's software renderer into the same pixels, on a
 * layer made over them as lv_canvas makes one, so the Arabic wraps of
 * lv_draw_label and lv_text_get_size (components/ui/src/arabic_text.c)
 * shape them as they do every label; each label's box is then made one bit
 * a pixel again.
 */
#include "map_overlay_priv.h"
#include "map_compose.h"

#include "esp_heap_caps.h"
#include "lvgl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORLD_ZOOM   17
#define WORLD_MAX    ((double)(256u << WORLD_ZOOM))
#define INK_R        1.5  /* px: lines 3 px wide */
#define CASING_R     3.5  /* px: in paper 7 px wide */
#define TEXT_BYTES   (MAP_OVERLAY_MAX_ITEMS * MAP_OVERLAY_LABEL_MAX)
#define ICON_R       7    /* the icon's paper halo; its ink is 5 */
#define LABEL_GAP    2    /* between the halo and the label's box */
#define LABEL_H      18
#define LABEL_PAD    2
#define LABEL_MAX_W  120
#define LINE_MARGIN  8    /* lines are clipped this far outside the view */

typedef struct {
    uint8_t  kind;         /* map_item_kind_t */
    uint8_t  icon;         /* map_icon_t, for a point */
    bool     drawn;        /* by the last draw: in view and not left out */
    bool     label_drawn;
    int16_t  label_w;      /* px, at most LABEL_MAX_W; 0: no label */
    int32_t  label;        /* offset in text, -1 for none */
    uint32_t first, count; /* its vertices */
    uint32_t id;
    lv_area_t box;         /* a point's halo or a text's box, last drawn */
    lv_area_t label_box;   /* a point's label, last drawn */
} item_t;

struct map_overlay {
    map_view_t          *view;
    map_overlay_t       *next;
    char                 name[MAP_OVERLAY_NAME_MAX];
    int                  z;
    bool                 visible;
    map_overlay_tap_cb_t tap;
    void                *user;
    item_t              *items;  /* MAP_OVERLAY_MAX_ITEMS, PSRAM */
    int                  count;
    double              *xy;     /* vertices, x then y, PSRAM, grown */
    uint32_t             vertices, vcap;
    char                *text;   /* TEXT_BYTES, PSRAM */
    uint32_t             text_used;
};

static void *alloc_psram(size_t len)
{
    void *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(len);
}

static const lv_font_t *label_font(void)
{
    return &lv_font_montserrat_14;
}

/* ------------------------------------------------------------------ */
/* Making and filling                                                   */
/* ------------------------------------------------------------------ */

map_overlay_t *map_overlay_create(map_view_t *view, const char *name, int z,
                                  map_overlay_tap_cb_t tap, void *user)
{
    if (!view) return NULL;
    map_overlay_t *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->items = alloc_psram(sizeof(item_t) * MAP_OVERLAY_MAX_ITEMS);
    o->text = alloc_psram(TEXT_BYTES);
    if (!o->items || !o->text) {
        free(o->items);
        free(o->text);
        free(o);
        return NULL;
    }
    snprintf(o->name, sizeof o->name, "%s", name ? name : "");
    o->view = view;
    o->z = z;
    o->visible = true;
    o->tap = tap;
    o->user = user;
    map_view_attach_overlay(view, o);
    return o;
}

void map_overlay_destroy(map_overlay_t *o)
{
    if (!o) return;
    map_view_detach_overlay(o->view, o);
    free(o->items);
    free(o->xy);
    free(o->text);
    free(o);
}

static bool reserve_vertices(map_overlay_t *o, uint32_t n)
{
    if (o->vertices + n > MAP_OVERLAY_MAX_VERTICES) return false;
    if (o->vertices + n <= o->vcap) return true;
    uint32_t cap = o->vcap ? o->vcap : 256;
    while (cap < o->vertices + n) cap *= 2;
    if (cap > MAP_OVERLAY_MAX_VERTICES) cap = MAP_OVERLAY_MAX_VERTICES;
    double *xy = heap_caps_realloc(o->xy, (size_t)cap * 2 * sizeof(double), MALLOC_CAP_SPIRAM);
    if (!xy) xy = realloc(o->xy, (size_t)cap * 2 * sizeof(double));
    if (!xy) return false;
    o->xy = xy;
    o->vcap = cap;
    return true;
}

static double world(double v)
{
    if (!(v >= 0)) return 0;  /* NaN too */
    return v > WORLD_MAX ? WORLD_MAX : v;
}

static void push_vertex(map_overlay_t *o, double lat, double lon)
{
    double x, y;
    map_world_pixel(lat, lon, WORLD_ZOOM, &x, &y);
    o->xy[o->vertices * 2] = world(x);
    o->xy[o->vertices * 2 + 1] = world(y);
    o->vertices++;
}

/* Copies text into the arena as one line, cut to MAP_OVERLAY_LABEL_MAX at a
 * character's end; its offset, or -1 for none. */
static int32_t keep_text(map_overlay_t *o, const char *text)
{
    if (!text || !*text) return -1;
    size_t n = strlen(text);
    if (n > MAP_OVERLAY_LABEL_MAX - 1) {
        n = MAP_OVERLAY_LABEL_MAX - 1;
        while (n > 0 && ((uint8_t)text[n] & 0xC0) == 0x80) n--;
    }
    if (o->text_used + n + 1 > TEXT_BYTES) return -1;
    char *out = o->text + o->text_used;
    for (size_t i = 0; i < n; i++) {
        out[i] = (text[i] == '\n' || text[i] == '\r' || text[i] == '\t') ? ' ' : text[i];
    }
    out[n] = '\0';
    int32_t at = (int32_t)o->text_used;
    o->text_used += (uint32_t)n + 1;
    return at;
}

static int text_width(const char *text)
{
    lv_point_t size;
    lv_text_get_size(&size, text, label_font(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x > LABEL_MAX_W ? LABEL_MAX_W : size.x;
}

static item_t *new_item(map_overlay_t *o, map_item_kind_t kind, uint32_t id)
{
    if (o->count >= MAP_OVERLAY_MAX_ITEMS) return NULL;
    item_t *it = &o->items[o->count];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind;
    it->id = id;
    it->label = -1;
    it->first = o->vertices;
    return it;
}

static int add_labelled(map_overlay_t *o, map_item_kind_t kind, double lat, double lon,
                        map_icon_t icon, const char *label, uint32_t id)
{
    if (!o || (kind == MAP_ITEM_TEXT && (!label || !*label))) return -1;
    item_t *it = new_item(o, kind, id);
    if (!it || !reserve_vertices(o, 1)) return -1;
    push_vertex(o, lat, lon);
    it->count = 1;
    it->icon = (uint8_t)((unsigned)icon < MAP_ICON_COUNT ? icon : MAP_ICON_DOT);
    it->label = keep_text(o, label);
    if (kind == MAP_ITEM_TEXT && it->label < 0) {
        o->vertices--;
        return -1;
    }
    if (it->label >= 0) it->label_w = (int16_t)text_width(o->text + it->label);
    return o->count++;
}

int map_overlay_add_point(map_overlay_t *o, double lat, double lon, map_icon_t icon,
                          const char *label, uint32_t id)
{
    return add_labelled(o, MAP_ITEM_POINT, lat, lon, icon, label, id);
}

int map_overlay_add_text(map_overlay_t *o, double lat, double lon, const char *text, uint32_t id)
{
    return add_labelled(o, MAP_ITEM_TEXT, lat, lon, MAP_ICON_DOT, text, id);
}

static int add_path(map_overlay_t *o, map_item_kind_t kind, const map_lat_lon_t *points,
                    int count, uint32_t id)
{
    int least = kind == MAP_ITEM_POLYGON ? 3 : 2;
    if (!o || !points || count < least) return -1;
    item_t *it = new_item(o, kind, id);
    if (!it || !reserve_vertices(o, (uint32_t)count)) return -1;
    for (int i = 0; i < count; i++) push_vertex(o, points[i].lat, points[i].lon);
    it->count = (uint32_t)count;
    return o->count++;
}

int map_overlay_add_line(map_overlay_t *o, const map_lat_lon_t *points, int count, uint32_t id)
{
    return add_path(o, MAP_ITEM_LINE, points, count, id);
}

int map_overlay_add_polygon(map_overlay_t *o, const map_lat_lon_t *points, int count,
                            uint32_t id)
{
    return add_path(o, MAP_ITEM_POLYGON, points, count, id);
}

int map_overlay_add_from_pack(map_overlay_t *o, const map_pack_t *pack, int overlay)
{
    int items = map_pack_overlay_items(pack, overlay);
    if (!o || !map_pack_overlay_name(pack, overlay)) return -1;
    int added = 0;
    for (int i = 0; i < items; i++) {
        map_pack_item_t pi;
        if (!map_pack_overlay_item(pack, overlay, i, &pi)) break;
        double lat, lon;
        int got = -1;
        if (pi.kind == MAP_ITEM_POINT || pi.kind == MAP_ITEM_TEXT) {
            if (map_pack_overlay_vertex(pack, overlay, i, 0, &lat, &lon)) {
                got = add_labelled(o, (map_item_kind_t)pi.kind, lat, lon, (map_icon_t)pi.icon,
                                   pi.label[0] ? pi.label : NULL, (uint32_t)i);
            }
        } else {
            /* Straight from the Pack's bytes: no buffer of points. */
            item_t *it = new_item(o, (map_item_kind_t)pi.kind, (uint32_t)i);
            if (it && reserve_vertices(o, (uint32_t)pi.vertices)) {
                for (int v = 0; v < pi.vertices; v++) {
                    map_pack_overlay_vertex(pack, overlay, i, v, &lat, &lon);
                    push_vertex(o, lat, lon);
                }
                it->count = (uint32_t)pi.vertices;
                got = o->count++;
            }
        }
        if (got >= 0) added++;
    }
    return added;
}

void map_overlay_clear(map_overlay_t *o)
{
    if (!o) return;
    o->count = 0;
    o->vertices = 0;
    o->text_used = 0;
}

void map_overlay_changed(map_overlay_t *o)
{
    if (o) map_view_overlay_changed(o->view, o);
}

void map_overlay_set_visible(map_overlay_t *o, bool visible)
{
    if (!o || o->visible == visible) return;
    o->visible = visible;
    map_view_overlay_switched(o->view, o);
}

bool map_overlay_visible(const map_overlay_t *o) { return o && o->visible; }
int map_overlay_count(const map_overlay_t *o) { return o ? o->count : 0; }
const char *map_overlay_name(const map_overlay_t *o) { return o ? o->name : ""; }
map_overlay_t *map_overlay_next(const map_overlay_t *o) { return o->next; }
void map_overlay_set_next(map_overlay_t *o, map_overlay_t *next) { o->next = next; }
int map_overlay_z(const map_overlay_t *o) { return o->z; }

void map_overlay_tap(map_overlay_t *o, int item, uint32_t id)
{
    if (o->tap) o->tap(o, item, id, o->user);
}

map_item_kind_t map_overlay_item_kind(const map_overlay_t *o, int item)
{
    if (!o || item < 0 || item >= o->count) return MAP_ITEM_POINT;
    return (map_item_kind_t)o->items[item].kind;
}

const char *map_overlay_item_label(const map_overlay_t *o, int item)
{
    if (!o || item < 0 || item >= o->count || o->items[item].label < 0) return "";
    return o->text + o->items[item].label;
}

/* ------------------------------------------------------------------ */
/* One bit a pixel                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t *px;
    int       w, h;
    int64_t   left, top;  /* the world pixel at (0, 0) */
} canvas_t;

static void put(const canvas_t *c, int x, int y, uint16_t colour)
{
    if ((unsigned)x < (unsigned)c->w && (unsigned)y < (unsigned)c->h) {
        c->px[(size_t)y * (size_t)c->w + (size_t)x] = colour;
    }
}

static void fill_rect(const canvas_t *c, int x1, int y1, int x2, int y2, uint16_t colour)
{
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= c->w) x2 = c->w - 1;
    if (y2 >= c->h) y2 = c->h - 1;
    for (int y = y1; y <= y2; y++) {
        uint16_t *row = c->px + (size_t)y * (size_t)c->w;
        for (int x = x1; x <= x2; x++) row[x] = colour;
    }
}

static void disc(const canvas_t *c, int cx, int cy, int r, uint16_t colour)
{
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r + r) put(c, cx + dx, cy + dy, colour);
        }
    }
}

static void icon(const canvas_t *c, int cx, int cy, map_icon_t kind)
{
    disc(c, cx, cy, ICON_R, MAP_PAPER);
    switch (kind) {
    case MAP_ICON_CIRCLE:
        disc(c, cx, cy, 5, MAP_INK);
        disc(c, cx, cy, 2, MAP_PAPER);
        break;
    case MAP_ICON_SQUARE:
        fill_rect(c, cx - 5, cy - 5, cx + 5, cy + 5, MAP_INK);
        break;
    case MAP_ICON_TRIANGLE:
        for (int dy = -5; dy <= 5; dy++) {
            int half = (dy + 5) / 2;
            fill_rect(c, cx - half, cy + dy, cx + half, cy + dy, MAP_INK);
        }
        break;
    case MAP_ICON_CROSS:
        fill_rect(c, cx - 1, cy - 5, cx + 1, cy + 5, MAP_INK);
        fill_rect(c, cx - 5, cy - 1, cx + 5, cy + 1, MAP_INK);
        break;
    case MAP_ICON_DOT:
    default:
        disc(c, cx, cy, 5, MAP_INK);
        break;
    }
}

/* Clips the segment to the view and LINE_MARGIN around it (Liang–Barsky);
 * false if none of it is there. */
static bool clip(const canvas_t *c, double *x0, double *y0, double *x1, double *y1)
{
    double lo_x = -LINE_MARGIN, lo_y = -LINE_MARGIN;
    double hi_x = c->w + LINE_MARGIN, hi_y = c->h + LINE_MARGIN;
    double dx = *x1 - *x0, dy = *y1 - *y0, t0 = 0, t1 = 1;
    double p[4] = {-dx, dx, -dy, dy};
    double q[4] = {*x0 - lo_x, hi_x - *x0, *y0 - lo_y, hi_y - *y0};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0) return false;
            continue;
        }
        double t = q[i] / p[i];
        if (p[i] < 0) {
            if (t > t1) return false;
            if (t > t0) t0 = t;
        } else {
            if (t < t0) return false;
            if (t < t1) t1 = t;
        }
    }
    double ax = *x0 + t0 * dx, ay = *y0 + t0 * dy;
    *x1 = *x0 + t1 * dx;
    *y1 = *y0 + t1 * dy;
    *x0 = ax;
    *y0 = ay;
    return true;
}

/* Sets each pixel whose centre is within radius of the segment. */
static void segment(const canvas_t *c, double ax, double ay, double bx, double by, double radius,
                    uint16_t colour)
{
    if (!clip(c, &ax, &ay, &bx, &by)) return;
    double x0 = fmin(ax, bx) - radius, x1 = fmax(ax, bx) + radius;
    double y0 = fmin(ay, by) - radius, y1 = fmax(ay, by) + radius;
    if (x1 < 0 || y1 < 0 || x0 >= c->w || y0 >= c->h) return;
    int sx0 = x0 < 0 ? 0 : (int)x0, sx1 = x1 >= c->w ? c->w - 1 : (int)x1;
    int sy0 = y0 < 0 ? 0 : (int)y0, sy1 = y1 >= c->h ? c->h - 1 : (int)y1;
    double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy, r2 = radius * radius;
    for (int y = sy0; y <= sy1; y++) {
        double py = y + 0.5;
        uint16_t *row = c->px + (size_t)y * (size_t)c->w;
        for (int x = sx0; x <= sx1; x++) {
            double px = x + 0.5;
            double t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            double ex = px - (ax + t * dx), ey = py - (ay + t * dy);
            if (ex * ex + ey * ey <= r2) row[x] = colour;
        }
    }
}

/* Vertex i of o in the canvas's pixels. */
static void vertex(const map_overlay_t *o, uint32_t i, int zoom, const canvas_t *c, double *x,
                   double *y)
{
    double scale = ldexp(1.0, zoom - WORLD_ZOOM);
    *x = o->xy[i * 2] * scale - (double)c->left;
    *y = o->xy[i * 2 + 1] * scale - (double)c->top;
}

/* Every segment of a line or a ring: each one's casing first, then each
 * one's ink, so a line that crosses itself keeps its ink. */
static void path(const map_overlay_t *o, const item_t *it, int zoom, const canvas_t *c,
                 double radius, uint16_t colour)
{
    uint32_t n = it->count, segs = it->kind == MAP_ITEM_POLYGON ? n : n - 1;
    for (uint32_t s = 0; s < segs; s++) {
        double x0, y0, x1, y1;
        vertex(o, it->first + s, zoom, c, &x0, &y0);
        vertex(o, it->first + (s + 1) % n, zoom, c, &x1, &y1);
        segment(c, x0, y0, x1, y1, radius, colour);
    }
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* A polygon's inside, even-odd, dotted every 4 px of the world. */
static void dots_inside(const map_overlay_t *o, const item_t *it, int zoom, const canvas_t *c)
{
    double *xs = malloc(sizeof(double) * it->count);
    if (!xs) return;
    for (int y = 0; y < c->h; y++) {
        if (((c->top + y) & 3) != 0) continue;
        double sy = y + 0.5;
        int n = 0;
        for (uint32_t s = 0; s < it->count; s++) {
            double x0, y0, x1, y1;
            vertex(o, it->first + s, zoom, c, &x0, &y0);
            vertex(o, it->first + (s + 1) % it->count, zoom, c, &x1, &y1);
            if ((y0 <= sy) != (y1 <= sy)) xs[n++] = x0 + (sy - y0) / (y1 - y0) * (x1 - x0);
        }
        qsort(xs, (size_t)n, sizeof *xs, cmp_double);
        for (int k = 0; k + 1 < n; k += 2) {
            int from = (int)ceil(xs[k] - 0.5), to = (int)floor(xs[k + 1] - 0.5);
            if (from < 0) from = 0;
            if (to >= c->w) to = c->w - 1;
            for (int x = from; x <= to; x++) {
                if (((c->left + x) & 3) == 0) put(c, x, y, MAP_INK);
            }
        }
    }
    free(xs);
}

/* Each label's box made one bit a pixel again, after LVGL drew it. */
static void threshold(const canvas_t *c, const lv_area_t *a)
{
    for (int y = a->y1 < 0 ? 0 : a->y1; y <= a->y2 && y < c->h; y++) {
        uint16_t *row = c->px + (size_t)y * (size_t)c->w;
        for (int x = a->x1 < 0 ? 0 : a->x1; x <= a->x2 && x < c->w; x++) {
            uint16_t p = row[x];
            unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
            unsigned lum = (r * 8 * 299 + g * 4 * 587 + b * 8 * 114) / 1000;
            row[x] = lum < 128 ? MAP_INK : MAP_PAPER;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                              */
/* ------------------------------------------------------------------ */

static bool overlap(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

static bool in_view(const lv_area_t *a, const canvas_t *c)
{
    return a->x2 >= 0 && a->y2 >= 0 && a->x1 < c->w && a->y1 < c->h;
}

/* A point's or a text's place: its centre, and the box it would take. */
static void place_of(const map_overlay_t *o, const item_t *it, int zoom, const canvas_t *c,
                     int *cx, int *cy, lv_area_t *box, lv_area_t *label_box)
{
    double x, y;
    vertex(o, it->first, zoom, c, &x, &y);
    /* Far off the view: kept in range, still off it. */
    if (x < -10000) x = -10000;
    if (x > 10000) x = 10000;
    if (y < -10000) y = -10000;
    if (y > 10000) y = 10000;
    *cx = (int)floor(x);
    *cy = (int)floor(y);
    if (it->kind == MAP_ITEM_TEXT) {
        int w = it->label_w + 2 * LABEL_PAD;
        *box = (lv_area_t){*cx - w / 2, *cy - LABEL_H / 2, *cx - w / 2 + w - 1,
                           *cy - LABEL_H / 2 + LABEL_H - 1};
        *label_box = *box;
        return;
    }
    *box = (lv_area_t){*cx - ICON_R, *cy - ICON_R, *cx + ICON_R, *cy + ICON_R};
    int lx = *cx + ICON_R + LABEL_GAP;
    *label_box = (lv_area_t){lx, *cy - LABEL_H / 2, lx + it->label_w + 2 * LABEL_PAD - 1,
                             *cy - LABEL_H / 2 + LABEL_H - 1};
}

/* Whether a line's or a polygon's vertices' box reaches into the view. */
static bool path_in_view(const map_overlay_t *o, const item_t *it, int zoom, const canvas_t *c)
{
    double x1 = INFINITY, y1 = INFINITY, x2 = -INFINITY, y2 = -INFINITY;
    for (uint32_t i = 0; i < it->count; i++) {
        double x, y;
        vertex(o, it->first + i, zoom, c, &x, &y);
        if (x < x1) x1 = x;
        if (x > x2) x2 = x;
        if (y < y1) y1 = y;
        if (y > y2) y2 = y;
    }
    return x2 >= -LINE_MARGIN && y2 >= -LINE_MARGIN && x1 < c->w + LINE_MARGIN &&
           y1 < c->h + LINE_MARGIN;
}

typedef struct {
    lv_area_t *rects;
    int        count, cap;
} taken_t;

static bool free_at(const taken_t *t, const lv_area_t *a)
{
    for (int i = 0; i < t->count; i++) {
        if (overlap(&t->rects[i], a)) return false;
    }
    return true;
}

static void take(taken_t *t, const lv_area_t *a)
{
    if (t->count < t->cap) t->rects[t->count++] = *a;
}

/* The overlays switched on, highest z first: n of them into out. */
static int on_highest_first(map_overlay_t *first, map_overlay_t **out, int cap)
{
    int n = 0;
    for (map_overlay_t *o = first; o && n < cap; o = o->next) {
        if (o->visible) out[n++] = o;
    }
    for (int i = 0; i < n / 2; i++) {
        map_overlay_t *t = out[i];
        out[i] = out[n - 1 - i];
        out[n - 1 - i] = t;
    }
    return n;
}

#define MAX_DRAWN_OVERLAYS 32

void map_overlays_draw(map_overlay_t *first, const map_frame_t *f, uint16_t *pixels,
                       map_overlay_stats_t *stats)
{
    memset(stats, 0, sizeof *stats);
    canvas_t c = {.px = pixels, .w = f->w, .h = f->h, .left = f->left, .top = f->top};
    for (map_overlay_t *o = first; o; o = o->next) {
        for (int i = 0; i < o->count; i++) o->items[i].drawn = o->items[i].label_drawn = false;
    }
    map_overlay_t *on[MAX_DRAWN_OVERLAYS];
    int n = on_highest_first(first, on, MAX_DRAWN_OVERLAYS);
    stats->on = n;
    if (n == 0) return;

    /* Placing: the highest overlays, and their earliest items, first. */
    int places = 0;
    for (int k = 0; k < n; k++) places += on[k]->count * 2;
    taken_t taken = {.rects = alloc_psram(sizeof(lv_area_t) * (size_t)(places ? places : 1)),
                     .cap = places};
    if (!taken.rects) taken.cap = 0;
    for (int k = 0; k < n; k++) {
        map_overlay_t *o = on[k];
        for (int i = 0; i < o->count; i++) {
            item_t *it = &o->items[i];
            if (it->kind == MAP_ITEM_LINE || it->kind == MAP_ITEM_POLYGON) {
                it->drawn = path_in_view(o, it, f->zoom, &c);
                stats->in_view += it->drawn;
                continue;
            }
            int cx, cy;
            place_of(o, it, f->zoom, &c, &cx, &cy, &it->box, &it->label_box);
            if (!in_view(&it->box, &c)) continue;
            stats->in_view++;
            if (!free_at(&taken, &it->box)) {
                stats->decluttered++;
                continue;
            }
            take(&taken, &it->box);
            it->drawn = true;
            if (it->kind == MAP_ITEM_POINT && it->label >= 0) {
                if (in_view(&it->label_box, &c) && free_at(&taken, &it->label_box)) {
                    take(&taken, &it->label_box);
                    it->label_drawn = true;
                }
            }
        }
    }
    free(taken.rects);

    /* Drawing: the lowest overlays first. */
    for (int k = n - 1; k >= 0; k--) {
        map_overlay_t *o = on[k];
        for (int pass = 0; pass < 3; pass++) {
            for (int i = 0; i < o->count; i++) {
                item_t *it = &o->items[i];
                if (!it->drawn) continue;
                if (pass == 0 && it->kind == MAP_ITEM_POLYGON) {
                    dots_inside(o, it, f->zoom, &c);
                    path(o, it, f->zoom, &c, CASING_R, MAP_PAPER);
                    path(o, it, f->zoom, &c, INK_R, MAP_INK);
                } else if (pass == 1 && it->kind == MAP_ITEM_LINE) {
                    path(o, it, f->zoom, &c, CASING_R, MAP_PAPER);
                    path(o, it, f->zoom, &c, INK_R, MAP_INK);
                } else if (pass == 2 && it->kind == MAP_ITEM_POINT) {
                    icon(&c, (it->box.x1 + it->box.x2) / 2, (it->box.y1 + it->box.y2) / 2,
                         (map_icon_t)it->icon);
                }
            }
        }
    }

    /* The labels and texts, on a layer over the same pixels. */
    lv_draw_buf_t buf;
    size_t bytes = (size_t)f->w * (size_t)f->h * sizeof(uint16_t);
    if (lv_draw_buf_init(&buf, (uint32_t)f->w, (uint32_t)f->h, LV_COLOR_FORMAT_RGB565,
                         (uint32_t)f->w * sizeof(uint16_t), pixels, (uint32_t)bytes) != LV_RESULT_OK) {
        return;
    }
    lv_layer_t layer;
    memset(&layer, 0, sizeof layer);
    lv_area_t all = {0, 0, f->w - 1, f->h - 1};
    layer.draw_buf = &buf;
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    layer.buf_area = all;
    layer._clip_area = all;
    layer.phy_clip_area = all;
#if LV_DRAW_TRANSFORM_USE_MATRIX
    lv_matrix_identity(&layer.matrix);
#endif
    for (int k = n - 1; k >= 0; k--) {
        map_overlay_t *o = on[k];
        for (int i = 0; i < o->count; i++) {
            item_t *it = &o->items[i];
            bool text = it->kind == MAP_ITEM_TEXT && it->drawn;
            if (!text && !it->label_drawn) continue;
            lv_area_t *a = text ? &it->box : &it->label_box;
            fill_rect(&c, a->x1, a->y1, a->x2, a->y2, MAP_PAPER);
            lv_draw_label_dsc_t d;
            lv_draw_label_dsc_init(&d);
            d.text = o->text + it->label;
            d.font = label_font();
            d.color = lv_color_black();
            d.bidi_dir = LV_BASE_DIR_AUTO;
            lv_area_t inner = {a->x1 + LABEL_PAD, a->y1 + 1, a->x2 - LABEL_PAD, a->y2};
            lv_area_t clipped = {
                inner.x1 > 0 ? inner.x1 : 0, inner.y1 > 0 ? inner.y1 : 0,
                inner.x2 < all.x2 ? inner.x2 : all.x2, inner.y2 < all.y2 ? inner.y2 : all.y2,
            };
            if (clipped.x1 > clipped.x2 || clipped.y1 > clipped.y2) continue;
            layer._clip_area = clipped;
            lv_draw_label(&layer, &d, &inner);
        }
    }
    layer._clip_area = all;
    lv_display_t *disp = lv_display_get_default();
    while (layer.draw_task_head && disp) {
        lv_draw_dispatch_wait_for_request();
        if (!lv_draw_dispatch_layer(disp, &layer)) {
            lv_draw_wait_for_finish();
            lv_draw_dispatch_request();
        }
    }
    for (int k = 0; k < n; k++) {
        map_overlay_t *o = on[k];
        for (int i = 0; i < o->count; i++) {
            item_t *it = &o->items[i];
            if (it->kind == MAP_ITEM_TEXT && it->drawn) threshold(&c, &it->box);
            else if (it->label_drawn) threshold(&c, &it->label_box);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Selection                                                            */
/* ------------------------------------------------------------------ */

static double to_rect(const lv_area_t *a, double x, double y)
{
    double dx = x < a->x1 ? a->x1 - x : x > a->x2 ? x - a->x2 : 0;
    double dy = y < a->y1 ? a->y1 - y : y > a->y2 ? y - a->y2 : 0;
    return sqrt(dx * dx + dy * dy);
}

static double to_segment(double px, double py, double x0, double y0, double x1, double y1)
{
    double dx = x1 - x0, dy = y1 - y0, len = dx * dx + dy * dy;
    double t = len > 0 ? ((px - x0) * dx + (py - y0) * dy) / len : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    double ex = x0 + t * dx - px, ey = y0 + t * dy - py;
    return sqrt(ex * ex + ey * ey);
}

static double to_path(const map_overlay_t *o, const item_t *it, int zoom, const canvas_t *c,
                      double px, double py)
{
    bool ring = it->kind == MAP_ITEM_POLYGON, inside = false;
    uint32_t n = it->count, segs = ring ? n : n - 1;
    double best = INFINITY;
    for (uint32_t s = 0; s < segs; s++) {
        double x0, y0, x1, y1;
        vertex(o, it->first + s, zoom, c, &x0, &y0);
        vertex(o, it->first + (s + 1) % n, zoom, c, &x1, &y1);
        double d = to_segment(px, py, x0, y0, x1, y1);
        if (d < best) best = d;
        if (ring && (y0 <= py) != (y1 <= py) && px < x0 + (py - y0) / (y1 - y0) * (x1 - x0)) {
            inside = !inside;
        }
    }
    return inside ? 0 : best;
}

map_overlay_hit_t map_overlays_hit(map_overlay_t *first, const map_frame_t *f, int x, int y)
{
    map_overlay_hit_t hit = {.overlay = NULL, .item = -1};
    canvas_t c = {.px = NULL, .w = f->w, .h = f->h, .left = f->left, .top = f->top};
    map_overlay_t *on[MAX_DRAWN_OVERLAYS];
    int n = on_highest_first(first, on, MAX_DRAWN_OVERLAYS);
    double best = MAP_OVERLAY_HIT_PX + 0.5;
    for (int k = 0; k < n; k++) {
        map_overlay_t *o = on[k];
        for (int i = 0; i < o->count; i++) {
            item_t *it = &o->items[i];
            if (!it->drawn) continue;
            double d;
            if (it->kind == MAP_ITEM_LINE || it->kind == MAP_ITEM_POLYGON) {
                d = to_path(o, it, f->zoom, &c, x, y);
            } else if (it->kind == MAP_ITEM_TEXT) {
                d = to_rect(&it->box, x, y);
            } else {
                double cx = (it->box.x1 + it->box.x2) / 2.0, cy = (it->box.y1 + it->box.y2) / 2.0;
                d = sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
                if (it->label_drawn) {
                    double l = to_rect(&it->label_box, x, y);
                    if (l < d) d = l;
                }
            }
            if (d < best) {
                best = d;
                hit = (map_overlay_hit_t){.overlay = o, .item = i, .id = it->id,
                                          .distance = (int)lround(d)};
            }
        }
    }
    return hit;
}
