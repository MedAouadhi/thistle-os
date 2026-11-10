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

typedef struct {
    uint32_t seq;  /* 0: nothing asked yet */
    int      zoom;
    int64_t  left, top;
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
    if (v->shown_cb) v->shown_cb(v, &shown, v->user);
}

static void ask(map_view_t *v, int zoom, int64_t left, int64_t top)
{
    lock(v);
    v->want.seq++;
    v->want.zoom = zoom;
    v->want.left = left;
    v->want.top = top;
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
            if (v->image) lv_obj_delete(v->image);
            return;
        }
    }
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
