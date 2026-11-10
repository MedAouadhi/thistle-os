/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — a map on the screen
 *
 * map_view.h says how it moves. Two tasks share a view:
 *
 *   UI task                          map_tiles task
 *   ───────                          ──────────────
 *   want = next view, seq + 1  ──▶   wakes, composes `want` into back
 *                                    (cache, Pack reads, inflates)
 *   poll timer: back ready and  ◀──  ready = true, if want is unchanged
 *   still wanted? swap front and
 *   back, invalidate the image:
 *   one flush, one refresh
 *
 * The lock guards want, ready and made; the buffers change hands only
 * through `ready`: the task writes back only while ready is false, and the
 * UI task touches back only to swap it in while ready is true. The Pack
 * and the cache are the task's alone once it runs.
 */
#include "map_engine/map_view.h"
#include "map_compose.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "map_view";

#define TASK_STACK    8192
#define TASK_PRIORITY 3    /* below lvgl_task (5) and the e-paper panel's (4) */
#define CACHE_SLOTS   12   /* 96 KB of PSRAM: two views' worth of tiles */
#define POLL_MS       20
#define SWIPE_MIN_PX  24   /* shorter is a tap, left for the app */
#define STOP_WAIT_MS  5000

/* The position marker: redrawn only once the fix has moved more than
 * MARKER_MOVE_PX on the screen, and at most every MARKER_GAP_MS; within
 * MARKER_EDGE_PX of the view's edge the map jumps a step. */
#define MARKER_PX       21   /* a side: a ring with a dot */
#define MARKER_RING_PX  3
#define MARKER_DOT_PX   7
#define MARKER_MOVE_PX  12
#define MARKER_GAP_MS   5000
#define MARKER_EDGE_PX  24

typedef struct {
    uint32_t seq;  /* 0: nothing asked yet */
    int      zoom;
    int64_t  left, top;
    /* The overlays shown, as they were when it was asked. */
    const map_overlay_t *overlays[MAP_VIEW_MAX_OVERLAYS];
    int      overlay_count;
} want_t;

struct map_view {
    lv_obj_t        *image;
    lv_image_dsc_t   dsc;
    lv_timer_t      *poll;
    uint16_t        *front;  /* w * h, in PSRAM: what the image shows */
    uint16_t        *back;   /* w * h, in PSRAM: what the task composes */
    int              w, h;
    map_pack_t      *pack;
    map_tile_cache_t *cache;
    map_view_shown_cb_t shown_cb;
    void            *user;
    lv_point_t       press_at;
    bool             pressing;
    int              bottom_inset;  /* px the app covers at the bottom */

    /* The app's overlays, the UI task's alone; each view asked for takes
     * the visible ones with it. */
    map_overlay_t   *overlays[MAP_VIEW_MAX_OVERLAYS];
    bool             overlay_on[MAP_VIEW_MAX_OVERLAYS];
    int              overlay_count;

    /* What is on the screen: the last view swapped in. */
    uint32_t         shown_seq;     /* 0: nothing yet */
    int              shown_zoom;
    int64_t          shown_left, shown_top;

    /* The position and its marker, the UI task's alone. */
    lv_obj_t        *marker;
    bool             has_pos;
    double           pos_lat, pos_lon;
    bool             marker_on;      /* drawn on the view */
    int              marker_x, marker_y;  /* its centre, in the view */
    int64_t          marker_us;      /* esp_timer when it was last drawn */
    bool             marker_ever;
    int              held_x, held_y; /* the last place said to be held */

    SemaphoreHandle_t lock;
    QueueHandle_t    wake;   /* to the task: something changed */
    QueueHandle_t    ended;  /* from the task: it has stopped */
    bool             task_running;
    bool             inline_compose;  /* no task: the UI task composes */
    uint32_t         done_seq;        /* the composer's own */
    /* Under the lock: */
    want_t           want;
    bool             ready;  /* back holds `made`, for want.seq made_seq */
    uint32_t         made_seq;
    map_view_shown_t made;
    bool             stop;
};

static void *alloc_psram(size_t len)
{
    void *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(len);
}

static void lock(map_view_t *v) { xSemaphoreTake(v->lock, portMAX_DELAY); }
static void unlock(map_view_t *v) { xSemaphoreGive(v->lock); }

static void wake(map_view_t *v)
{
    if (!v->wake) return;
    uint8_t token = 1;
    xQueueSend(v->wake, &token, 0);  /* full: it is woken already */
}

/* ------------------------------------------------------------------ */
/* Memory, as the hardware check reads it                               */
/* ------------------------------------------------------------------ */

static void log_internal_ram(const char *when)
{
#ifdef SIMULATOR_BUILD
    ESP_LOGI(TAG, "%s; internal RAM is measured on a device", when);
#else
    ESP_LOGI(TAG, "%s; internal RAM %u KB free, largest block %u KB", when,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
#endif
}

static void log_stack(const char *when)
{
#ifdef SIMULATOR_BUILD
    ESP_LOGI(TAG, "tile task %s; its stack is measured on a device", when);
#else
    ESP_LOGI(TAG, "tile task %s; %u bytes of its %u-byte stack never used", when,
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)TASK_STACK);
#endif
}

/* ------------------------------------------------------------------ */
/* The tile task                                                        */
/* ------------------------------------------------------------------ */

/* Composes want into back, and marks it ready if it is still wanted. */
static void compose(map_view_t *v, want_t want)
{
    int64_t started = esp_timer_get_time();
    map_compose_stats_t st;
    map_compose(v->cache, v->pack, want.zoom, want.left, want.top, v->w, v->h, v->back, &st);
    for (int i = 0; i < want.overlay_count; i++) {
        map_overlay_draw(want.overlays[i], want.zoom, want.left, want.top, v->w, v->h, v->back);
    }
    v->done_seq = want.seq;
    uint32_t ms = (uint32_t)((esp_timer_get_time() - started + 999) / 1000);

    lock(v);
    if (v->want.seq == want.seq) {
        v->made = (map_view_shown_t){
            .zoom = want.zoom, .left = want.left, .top = want.top,
            .tiles = st.tiles, .missing = st.missing, .damaged = st.damaged,
            .read = st.read, .cached = st.cached, .compose_ms = ms,
        };
        v->made_seq = want.seq;
        v->ready = true;
    }
    unlock(v);
}

static void tile_task(void *arg)
{
    map_view_t *v = arg;
    bool measured = false;
    for (;;) {
        uint8_t token;
        xQueueReceive(v->wake, &token, pdMS_TO_TICKS(1000));
        for (;;) {
            lock(v);
            bool stop = v->stop;
            want_t want = v->want;
            bool busy = v->ready;  /* back is waiting to be swapped in */
            unlock(v);
            if (stop) goto out;
            if (busy || want.seq == 0 || want.seq == v->done_seq) break;
            compose(v, want);
            if (!measured) {
                log_stack("after its first view");
                measured = true;
            }
        }
    }
out:
    log_stack("stopping");
    uint8_t token = 1;
    xQueueSend(v->ended, &token, 0);
    vTaskDelete(NULL);
}


/* ------------------------------------------------------------------ */
/* The position marker                                                  */
/* ------------------------------------------------------------------ */

/* Where the position is on the view now on the screen, its pixel. */
static void marker_spot(const map_view_t *v, int64_t *x, int64_t *y)
{
    double gx, gy;
    map_world_pixel(v->pos_lat, v->pos_lon, v->shown_zoom, &gx, &gy);
    *x = (int64_t)floor(gx + 0.5) - v->shown_left;
    *y = (int64_t)floor(gy + 0.5) - v->shown_top;
}

static int usable_h(const map_view_t *v)
{
    return v->h - v->bottom_inset;
}

static bool on_view(const map_view_t *v, int64_t x, int64_t y)
{
    return x >= 0 && x < v->w && y >= 0 && y < usable_h(v);
}

/* Within MARKER_EDGE_PX of an edge, or off the view. */
static bool near_edge(const map_view_t *v, int64_t x, int64_t y)
{
    return x < MARKER_EDGE_PX || x >= v->w - MARKER_EDGE_PX || y < MARKER_EDGE_PX ||
           y >= usable_h(v) - MARKER_EDGE_PX;
}

static void draw_marker(map_view_t *v, int x, int y)
{
    lv_obj_set_pos(v->marker, x - MARKER_PX / 2, y - MARKER_PX / 2);
    lv_obj_remove_flag(v->marker, LV_OBJ_FLAG_HIDDEN);
    v->marker_on = true;
    v->marker_x = x;
    v->marker_y = y;
    v->held_x = x;
    v->held_y = y;
    v->marker_us = esp_timer_get_time();
    v->marker_ever = true;
}

/* Puts the marker where the position is on the view just swapped in. */
static void place_marker(map_view_t *v)
{
    if (!v->has_pos || !v->marker) return;
    int64_t x, y;
    marker_spot(v, &x, &y);
    if (on_view(v, x, y)) {
        draw_marker(v, (int)x, (int)y);
        ESP_LOGI(TAG, "marker at %d,%d", (int)x, (int)y);
    } else {
        if (v->marker_on) lv_obj_add_flag(v->marker, LV_OBJ_FLAG_HIDDEN);
        v->marker_on = false;
        ESP_LOGI(TAG, "marker off the view");
    }
}

static lv_obj_t *make_marker(lv_obj_t *parent)
{
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, MARKER_PX, MARKER_PX);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ring, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, MARKER_RING_PX, LV_PART_MAIN);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *dot = lv_obj_create(ring);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, MARKER_DOT_PX, MARKER_DOT_PX);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(dot);
    return ring;
}

/* ------------------------------------------------------------------ */
/* The UI task's side                                                   */
/* ------------------------------------------------------------------ */

static void poll_cb(lv_timer_t *timer)
{
    map_view_t *v = lv_timer_get_user_data(timer);
    if (v->inline_compose && !v->ready && v->want.seq != 0 && v->want.seq != v->done_seq) {
        compose(v, v->want);
    }
    lock(v);
    bool ready = v->ready;
    bool wanted = ready && v->made_seq == v->want.seq;
    map_view_shown_t shown = v->made;
    if (wanted) {
        uint16_t *t = v->front;
        v->front = v->back;
        v->back = t;
    }
    v->ready = false;
    unlock(v);
    if (!ready) return;
    wake(v);  /* for what was asked meanwhile, if anything */
    if (!wanted) return;  /* overtaken: never drawn */

    lv_image_cache_drop(&v->dsc);
    v->dsc.data = (const uint8_t *)v->front;
    lv_image_set_src(v->image, &v->dsc);
    lv_obj_invalidate(v->image);
    v->shown_seq = v->made_seq;
    v->shown_zoom = shown.zoom;
    v->shown_left = shown.left;
    v->shown_top = shown.top;
    /* In the same pass as the map: the panel refreshes once for both. */
    place_marker(v);
    if (v->shown_cb) v->shown_cb(v, &shown, v->user);
}

static void ask(map_view_t *v, int zoom, int64_t left, int64_t top)
{
    lock(v);
    v->want.seq++;
    v->want.zoom = zoom;
    v->want.left = left;
    v->want.top = top;
    v->want.overlay_count = 0;
    for (int i = 0; i < v->overlay_count; i++) {
        if (v->overlay_on[i]) v->want.overlays[v->want.overlay_count++] = v->overlays[i];
    }
    unlock(v);
    wake(v);
}

static const char *direction(int dx, int dy)
{
    if (dx > 0) return "east";
    if (dx < 0) return "west";
    return dy > 0 ? "south" : "north";
}

static void key_cb(lv_event_t *e)
{
    map_view_t *v = lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);
    switch (key) {
    case LV_KEY_RIGHT: case 'd': case 'D': map_view_pan(v, 1, 0); break;
    case LV_KEY_LEFT:  case 'a': case 'A': map_view_pan(v, -1, 0); break;
    case LV_KEY_DOWN:  case 's': case 'S': map_view_pan(v, 0, 1); break;
    case LV_KEY_UP:    case 'w': case 'W': map_view_pan(v, 0, -1); break;
    case '+': case '=': case 'i': case 'I': map_view_zoom_by(v, 1); break;
    case '-': case 'o': case 'O': map_view_zoom_by(v, -1); break;
    default: return;  /* the app's */
    }
}

/* A swipe drags the map: the view moves the other way, a step. */
static void swipe(map_view_t *v, lv_indev_t *indev)
{
    if (!v->pressing || !indev) return;
    v->pressing = false;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int dx = p.x - v->press_at.x, dy = p.y - v->press_at.y;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ax < SWIPE_MIN_PX && ay < SWIPE_MIN_PX) return;
    if (ax >= ay) map_view_pan(v, dx < 0 ? 1 : -1, 0);
    else map_view_pan(v, 0, dy < 0 ? 1 : -1);
}

static void touch_cb(lv_event_t *e)
{
    map_view_t *v = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_get_point(indev, &v->press_at);
            v->pressing = true;
        }
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        swipe(v, indev);
        break;
    default:
        break;
    }
}

map_view_t *map_view_create(lv_obj_t *parent, map_pack_t *pack, map_view_shown_cb_t shown,
                            void *user)
{
    lv_obj_update_layout(parent);
    int w = lv_obj_get_content_width(parent);
    int h = lv_obj_get_content_height(parent);
    if (w <= 0 || h <= 0) return NULL;

    log_internal_ram("before the map view");
    map_view_t *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->w = w;
    v->h = h;
    v->pack = pack;
    v->shown_cb = shown;
    v->user = user;
    v->front = alloc_psram((size_t)w * h * sizeof(uint16_t));
    v->back = alloc_psram((size_t)w * h * sizeof(uint16_t));
    v->cache = map_tile_cache_create(CACHE_SLOTS);
    v->lock = xSemaphoreCreateMutex();
#ifdef __EMSCRIPTEN__
    /* The browser's simulator runs no tasks. */
    v->inline_compose = true;
#else
    v->wake = xQueueCreate(1, sizeof(uint8_t));
    v->ended = xQueueCreate(1, sizeof(uint8_t));
#endif
    if (!v->front || !v->back || !v->cache || !v->lock ||
        (!v->inline_compose && (!v->wake || !v->ended))) {
        map_view_destroy(v);
        return NULL;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) v->front[i] = MAP_PAPER;

    v->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    v->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    v->dsc.header.w = (uint32_t)w;
    v->dsc.header.h = (uint32_t)h;
    v->dsc.header.stride = (uint32_t)w * sizeof(uint16_t);
    v->dsc.data_size = (uint32_t)((size_t)w * h * sizeof(uint16_t));
    v->dsc.data = (const uint8_t *)v->front;

    v->image = lv_image_create(parent);
    /* No theme: a press or the keyboard's focus would draw, a refresh of
     * its own on e-paper. */
    lv_obj_remove_style_all(v->image);
    lv_obj_remove_flag(v->image, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(v->image, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(v->image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(v->image, 0, 0);
    lv_obj_set_size(v->image, w, h);
    lv_image_set_src(v->image, &v->dsc);
    lv_obj_add_event_cb(v->image, touch_cb, LV_EVENT_PRESSED, v);
    lv_obj_add_event_cb(v->image, touch_cb, LV_EVENT_RELEASED, v);
    lv_obj_add_event_cb(v->image, touch_cb, LV_EVENT_PRESS_LOST, v);
    lv_obj_add_event_cb(v->image, key_cb, LV_EVENT_KEY, v);
    lv_group_t *group = lv_group_get_default();
    if (group) {
        lv_group_add_obj(group, v->image);
        lv_group_focus_obj(v->image);
    }
    v->marker = make_marker(parent);

    if (!v->inline_compose) {
        if (xTaskCreate(tile_task, "map_tiles", TASK_STACK, v, TASK_PRIORITY, NULL) != pdPASS) {
            ESP_LOGE(TAG, "no memory for the tile task");
            map_view_destroy(v);
            return NULL;
        }
        v->task_running = true;
    }
    v->poll = lv_timer_create(poll_cb, POLL_MS, v);
    log_internal_ram("map view made, its tile task started");
    return v;
}

void map_view_destroy(map_view_t *v)
{
    if (!v) return;
    if (v->poll) lv_timer_delete(v->poll);
    if (v->task_running) {
        lock(v);
        v->stop = true;
        unlock(v);
        wake(v);
        uint8_t token;
        if (xQueueReceive(v->ended, &token, pdMS_TO_TICKS(STOP_WAIT_MS)) != pdTRUE) {
            /* Stuck reading the card: freeing the view under it would be
             * worse than keeping it. */
            ESP_LOGE(TAG, "the tile task did not stop; its view is kept");
            if (v->marker) lv_obj_delete(v->marker);
            if (v->image) lv_obj_delete(v->image);
            return;
        }
    }
    if (v->marker) lv_obj_delete(v->marker);
    if (v->image) {
        lv_image_cache_drop(&v->dsc);
        lv_obj_delete(v->image);
    }
    map_tile_cache_destroy(v->cache);
    free(v->front);
    free(v->back);
    if (v->wake) vQueueDelete(v->wake);
    if (v->ended) vQueueDelete(v->ended);
    if (v->lock) vSemaphoreDelete(v->lock);
    free(v);
}

lv_obj_t *map_view_obj(map_view_t *view) { return view->image; }
int map_view_width(const map_view_t *view) { return view->w; }
int map_view_height(const map_view_t *view) { return view->h; }

static int clamp_zoom(int zoom)
{
    return zoom < MAP_MIN_ZOOM ? MAP_MIN_ZOOM : zoom > MAP_MAX_ZOOM ? MAP_MAX_ZOOM : zoom;
}

void map_view_go_to(map_view_t *v, double lat, double lon, int zoom)
{
    zoom = clamp_zoom(zoom);
    double gx, gy;
    map_world_pixel(lat, lon, zoom, &gx, &gy);
    ask(v, zoom, (int64_t)floor(gx + 0.5) - v->w / 2, (int64_t)floor(gy + 0.5) - v->h / 2);
}

void map_view_pan(map_view_t *v, int dx, int dy)
{
    if (dx == 0 && dy == 0) return;
    lock(v);
    want_t w = v->want;
    unlock(v);
    if (w.seq == 0) return;  /* nowhere yet */
    int64_t left = w.left + (int64_t)dx * (v->w / 2), top = w.top + (int64_t)dy * (v->h / 2);
    ESP_LOGI(TAG, "pan %s: z%d at %lld,%lld", direction(dx, dy), w.zoom, (long long)left,
             (long long)top);
    ask(v, w.zoom, left, top);
}

/* v * 2^levels, floored, for negative v too. */
static int64_t scale(int64_t v, int levels)
{
    if (levels >= 0) return v * ((int64_t)1 << levels);
    int64_t d = (int64_t)1 << -levels;
    return v >= 0 ? v / d : -((-v + d - 1) / d);
}

bool map_view_zoom_by(map_view_t *v, int levels)
{
    lock(v);
    want_t w = v->want;
    unlock(v);
    if (w.seq == 0) return false;
    int zoom = clamp_zoom(w.zoom + levels);
    if (zoom == w.zoom) {
        ESP_LOGI(TAG, "already at z%d", w.zoom);
        return false;
    }
    int64_t cx = scale(w.left + v->w / 2, zoom - w.zoom);
    int64_t cy = scale(w.top + v->h / 2, zoom - w.zoom);
    int64_t left = cx - v->w / 2, top = cy - v->h / 2;
    ESP_LOGI(TAG, "zoom %s: z%d at %lld,%lld", zoom > w.zoom ? "in" : "out", zoom,
             (long long)left, (long long)top);
    ask(v, zoom, left, top);
    return true;
}

int map_view_zoom(const map_view_t *v)
{
    return v->want.zoom;
}

void map_view_origin(const map_view_t *v, int64_t *x, int64_t *y)
{
    *x = v->want.left;
    *y = v->want.top;
}

void map_view_set_bottom_inset(map_view_t *v, int px)
{
    v->bottom_inset = px < 0 ? 0 : px >= v->h ? v->h - 1 : px;
}

static const char *compass(int dx, int dy)
{
    static const char *const names[3][3] = {
        { "north-west", "north", "north-east" },
        { "west",       "here",  "east" },
        { "south-west", "south", "south-east" },
    };
    return names[dy + 1][dx + 1];
}

/* -1, 0 or 1: which way a spot in the edge band lies. */
static int side(int64_t at, int size)
{
    return at < MARKER_EDGE_PX ? -1 : at >= size - MARKER_EDGE_PX ? 1 : 0;
}

static void say_held(map_view_t *v, int64_t x, int64_t y, uint32_t since)
{
    if (x == v->held_x && y == v->held_y) return;  /* said already */
    v->held_x = (int)x;
    v->held_y = (int)y;
    int moved = (int)lround(hypot((double)(x - v->marker_x), (double)(y - v->marker_y)));
    ESP_LOGI(TAG, "marker held: %d px from where it is drawn, %.1f s after the last", moved,
             since / 1000.0);
}

void map_view_set_position(map_view_t *v, double lat, double lon)
{
    v->has_pos = true;
    v->pos_lat = lat;
    v->pos_lon = lon;
    lock(v);
    bool pending = v->want.seq != v->shown_seq;
    unlock(v);
    /* A view on its way places the marker as it is swapped in. */
    if (pending || v->shown_seq == 0) return;

    int64_t x, y;
    marker_spot(v, &x, &y);
    /* esp_timer, not lv_tick: the simulator's lv_tick runs fast. */
    uint32_t since = v->marker_ever
                         ? (uint32_t)((esp_timer_get_time() - v->marker_us) / 1000)
                         : MARKER_GAP_MS;
    bool following = v->marker_on && !near_edge(v, v->marker_x, v->marker_y);

    if (following && near_edge(v, x, y)) {
        if (since < MARKER_GAP_MS) {
            say_held(v, x, y, since);
            return;
        }
        int dx = side(x, v->w), dy = side(y, usable_h(v));
        int64_t left = v->shown_left + (int64_t)dx * (v->w / 2);
        int64_t top = v->shown_top + (int64_t)dy * (v->h / 2);
        int64_t nx = x - (left - v->shown_left), ny = y - (top - v->shown_top);
        if (!near_edge(v, nx, ny)) {
            ESP_LOGI(TAG, "page jump %s to keep the marker in view: z%d at %lld,%lld",
                     compass(dx, dy), v->shown_zoom, (long long)left, (long long)top);
        } else {
            /* Further than a step: the fix itself in the middle. */
            left = v->shown_left + x - v->w / 2;
            top = v->shown_top + y - usable_h(v) / 2;
            ESP_LOGI(TAG, "page jump to the marker, a step being too short: z%d at %lld,%lld",
                     v->shown_zoom, (long long)left, (long long)top);
        }
        ask(v, v->shown_zoom, left, top);
        return;
    }

    if (!on_view(v, x, y)) {
        if (v->marker_on) {
            /* Gone off the view from its edge band: not followed. */
            lv_obj_add_flag(v->marker, LV_OBJ_FLAG_HIDDEN);
            v->marker_on = false;
            ESP_LOGI(TAG, "marker off the view");
        }
        return;
    }
    if (!v->marker_on) {
        if (since < MARKER_GAP_MS) {
            say_held(v, x, y, since);
            return;
        }
        draw_marker(v, (int)x, (int)y);
        ESP_LOGI(TAG, "marker at %d,%d", (int)x, (int)y);
        return;
    }
    int moved = (int)lround(hypot((double)(x - v->marker_x), (double)(y - v->marker_y)));
    if (moved <= MARKER_MOVE_PX || since < MARKER_GAP_MS) {
        say_held(v, x, y, since);
        return;
    }
    draw_marker(v, (int)x, (int)y);
    ESP_LOGI(TAG, "marker moved %d px to %d,%d, %.1f s after the last", moved, (int)x, (int)y,
             since / 1000.0);
}

bool map_view_locate(map_view_t *v)
{
    if (!v->has_pos) return false;
    lock(v);
    int zoom = v->want.seq ? v->want.zoom : MAP_MAX_ZOOM - 1;
    unlock(v);
    double gx, gy;
    map_world_pixel(v->pos_lat, v->pos_lon, zoom, &gx, &gy);
    int64_t left = (int64_t)floor(gx + 0.5) - v->w / 2;
    int64_t top = (int64_t)floor(gy + 0.5) - v->h / 2;
    ESP_LOGI(TAG, "locate: z%d at %lld,%lld", zoom, (long long)left, (long long)top);
    ask(v, zoom, left, top);
    return true;
}

bool map_view_add_overlay(map_view_t *v, map_overlay_t *overlay, bool visible)
{
    if (!overlay || v->overlay_count >= MAP_VIEW_MAX_OVERLAYS) return false;
    v->overlays[v->overlay_count] = overlay;
    v->overlay_on[v->overlay_count] = visible;
    v->overlay_count++;
    lock(v);
    want_t w = v->want;
    unlock(v);
    if (visible && w.seq != 0) ask(v, w.zoom, w.left, w.top);
    return true;
}

void map_view_set_overlay_visible(map_view_t *v, map_overlay_t *overlay, bool visible)
{
    for (int i = 0; i < v->overlay_count; i++) {
        if (v->overlays[i] != overlay || v->overlay_on[i] == visible) continue;
        v->overlay_on[i] = visible;
        lock(v);
        want_t w = v->want;
        unlock(v);
        if (w.seq == 0) return;
        ESP_LOGI(TAG, "overlay %s %s: z%d at %lld,%lld", map_overlay_name(overlay),
                 visible ? "on" : "off", w.zoom, (long long)w.left, (long long)w.top);
        ask(v, w.zoom, w.left, w.top);
        return;
    }
}

bool map_view_overlay_visible(const map_view_t *v, const map_overlay_t *overlay)
{
    for (int i = 0; i < v->overlay_count; i++) {
        if (v->overlays[i] == overlay) return v->overlay_on[i];
    }
    return false;
}
