/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Transit app's fetching. See transit_session.h.
 *
 * A session is one allocation in PSRAM: what the UI asked for, the last
 * result, a result being filled, and the provider's own state. The task
 * owns the fetching; the UI reaches the rest through s_lock and s_current.
 * Stopping detaches the session from s_current under the lock, so nothing
 * else reaches it after, and the task frees it as it ends, as the Hermes
 * app's Bridge Session does: a fetch held up on the network costs a stop
 * nothing, and none starts after.
 */
#include "transit/transit_session.h"

#include "thistle/power_manager.h"
#include "hal/sdcard_path.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "transit";

#define TASK_STACK    8192
#define TASK_PRIORITY 4
#define POLL_MS       100
#define MAPS_DIR      THISTLE_SDCARD "/maps"
#define OVERLAYS_DIR  MAPS_DIR "/overlays"

typedef struct {
    volatile bool    stop;
    transit_config_t config;
    /* Asked by the UI, under the lock. */
    bool             have_box;
    transit_box_t    box;
    char             stop_id[TRANSIT_ID_MAX];
    bool             force;
    transit_why_t    force_why;
    /* Told back, under the lock. */
    transit_status_t status;
    transit_result_t result;
    /* The task's own. */
    transit_result_t work;
    void            *provider_state;
} session_t;

static SemaphoreHandle_t s_lock;
static session_t *s_current;

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void *psram_calloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!p) p = malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

static const char *why_words(transit_why_t why, int interval_s, char *buf, size_t cap)
{
    switch (why) {
    case TRANSIT_WHY_REFRESH: return "Refresh";
    case TRANSIT_WHY_STOP:    return "a stop tapped";
    case TRANSIT_WHY_OPENING: return "the app opening";
    default:
        snprintf(buf, cap, "the %d s interval", interval_s);
        return buf;
    }
}

/* ------------------------------------------------------------------ */
/* The last result as GeoJSON                                           */
/* ------------------------------------------------------------------ */

static void put_json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void put_point(FILE *f, bool *first, double lat, double lon, const char *symbol,
                      const char *name, const char *key, const char *value)
{
    fputs(*first ? "\n" : ",\n", f);
    *first = false;
    fprintf(f, "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":"
               "[%.6f,%.6f]},\"properties\":{\"marker-symbol\":\"%s\",\"name\":",
            lon, lat, symbol);
    put_json_string(f, name);
    if (value && value[0]) {
        fprintf(f, ",\"%s\":", key);
        put_json_string(f, value);
    }
    fputs("}}", f);
}

/* Written whole by way of a .tmp, so the Maps app never reads half. */
static void save_geojson(const session_t *s)
{
    char path[sizeof OVERLAYS_DIR + 48], tmp[sizeof path + 4];
    snprintf(path, sizeof path, "%s/transit-%s.geojson", OVERLAYS_DIR, s->config.provider->id);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    mkdir(MAPS_DIR, 0755);       /* either may be there already */
    mkdir(OVERLAYS_DIR, 0755);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        ESP_LOGW(TAG, "cannot write %s", path);
        return;
    }
    const transit_result_t *r = &s->work;
    bool first = true;
    fputs("{\"type\":\"FeatureCollection\",\"features\":[", f);
    for (int i = 0; i < r->vehicle_count; i++) {
        const transit_vehicle_t *v = &r->vehicles[i];
        put_point(f, &first, v->lat, v->lon, "triangle", v->line, "direction", v->direction);
    }
    for (int i = 0; i < r->stop_count; i++) {
        const transit_stop_t *st = &r->stops[i];
        put_point(f, &first, st->lat, st->lon, "square", st->name, "stop", st->id);
    }
    fputs("\n]}\n", f);
    bool ok = !ferror(f);
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        ESP_LOGW(TAG, "cannot write %s", path);
        return;
    }
    ESP_LOGI(TAG, "saved the last result: %d vehicles, %d stops in maps/overlays/transit-%s.geojson",
             r->vehicle_count, r->stop_count, s->config.provider->id);
}

/* ------------------------------------------------------------------ */
/* The task                                                             */
/* ------------------------------------------------------------------ */

static void log_stack(void)
{
#ifdef SIMULATOR_BUILD
    ESP_LOGI(TAG, "fetch task's stack is measured on a device");
#else
    ESP_LOGI(TAG, "%u bytes of the fetch task's %u-byte stack never used",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)TASK_STACK);
#endif
}

static void session_task(void *arg)
{
    session_t *s = arg;
    const transit_provider_t *p = s->config.provider;
    int64_t next_us = 0, last_us = 0;
    int failures = 0;
    bool measured = false;
    ESP_LOGI(TAG, "%s: fetching every %d s while the app is open", p->id, s->config.interval_s);

    while (!s->stop) {
        int64_t now_us = esp_timer_get_time();
        lock();
        bool ready = s->have_box;
        bool forced = s->force && now_us - last_us >= TRANSIT_FORCED_GAP_MS * 1000LL;
        bool due = ready && (forced || (now_us >= next_us && !s->force));
        if (s->force && !forced) due = false;
        transit_request_t rq;
        transit_why_t why = last_us ? TRANSIT_WHY_INTERVAL : TRANSIT_WHY_OPENING;
        if (due) {
            memset(&rq, 0, sizeof rq);
            rq.box = s->box;
            snprintf(rq.stop_id, sizeof rq.stop_id, "%s", s->stop_id);
            snprintf(rq.base_url, sizeof rq.base_url, "%s", s->config.base_url);
            time_t t = time(NULL);
            rq.now = t > 1767225600 ? (int64_t)t : 0;  /* 2026, as the clock's floor */
            if (forced) why = s->force_why;
            s->force = false;
        }
        unlock();
        if (!due) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }

        uint32_t hold = power_manager_hold("Transit is fetching");
        memset(&s->work, 0, sizeof s->work);
        transit_error_t error = {0};
        int64_t started = esp_timer_get_time();
        bool ok = p->fetch(s->provider_state, &rq, &s->work, &error);
        int64_t took_ms = (esp_timer_get_time() - started) / 1000;
        power_manager_release(hold);
        last_us = esp_timer_get_time();
        if (s->stop) break;

        char buf[32];
        int retry_s = s->config.interval_s;
        if (ok) {
            failures = 0;
            ESP_LOGI(TAG, "%s: fetched %d stops, %d vehicles, %d departures in %lld ms, for %s",
                     p->id, s->work.stop_count, s->work.vehicle_count, s->work.departure_count,
                     (long long)took_ms, why_words(why, s->config.interval_s, buf, sizeof buf));
        } else {
            failures++;
            for (int i = 1; i < failures && retry_s < TRANSIT_BACKOFF_MAX_S; i++) retry_s *= 2;
            if (retry_s > TRANSIT_BACKOFF_MAX_S) retry_s = TRANSIT_BACKOFF_MAX_S;
            if (error.http_status) {
                ESP_LOGW(TAG, "%s: fetch failed: HTTP %d, %s; trying again in %d s", p->id,
                         error.http_status, error.words, retry_s);
            } else {
                ESP_LOGW(TAG, "%s: fetch failed: %s; trying again in %d s", p->id, error.words,
                         retry_s);
            }
        }
        next_us = last_us + (int64_t)retry_s * 1000000;

        lock();
        s->status.seq++;
        s->status.ok = ok;
        s->status.why = why;
        s->status.error = error;
        s->status.retry_s = retry_s;
        if (ok) {
            s->status.ok_at_us = last_us;
            memcpy(&s->result, &s->work, sizeof s->result);
        }
        unlock();

        if (ok && s->config.save_geojson) save_geojson(s);
        if (!measured) {
            log_stack();
            measured = true;
        }
    }

    /* Detached by transit_session_stop() before `stop` was set: nobody
     * else reaches it now. */
    ESP_LOGI(TAG, "%s: stopped fetching", p->id);
    free(s->provider_state);
    free(s);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* The UI's side                                                        */
/* ------------------------------------------------------------------ */

bool transit_session_start(const transit_config_t *config)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return false;
    }
    transit_session_stop();
    session_t *s = psram_calloc(sizeof *s);
    if (!s) return false;
    s->config = *config;
    if (s->config.interval_s < 1) s->config.interval_s = config->provider->min_interval_s;
    if (config->provider->state_size) {
        s->provider_state = psram_calloc(config->provider->state_size);
        if (!s->provider_state) {
            free(s);
            return false;
        }
    }
    lock();
    s_current = s;
    unlock();
    /* The stack in internal RAM, as every task's (CLAUDE.md). */
    if (xTaskCreate(session_task, "transit", TASK_STACK, s, TASK_PRIORITY, NULL) != pdPASS) {
        lock();
        s_current = NULL;
        unlock();
        free(s->provider_state);
        free(s);
        ESP_LOGE(TAG, "no memory for the fetch task");
        return false;
    }
    return true;
}

void transit_session_stop(void)
{
    if (!s_lock) return;
    lock();
    session_t *s = s_current;
    s_current = NULL;
    if (s) s->stop = true;
    unlock();
}

void transit_session_set_box(const transit_box_t *box)
{
    if (!s_lock) return;
    lock();
    if (s_current) {
        s_current->box = *box;
        s_current->have_box = true;
    }
    unlock();
}

void transit_session_select_stop(const char *stop_id)
{
    if (!s_lock) return;
    lock();
    if (s_current) {
        snprintf(s_current->stop_id, sizeof s_current->stop_id, "%s", stop_id ? stop_id : "");
        if (s_current->stop_id[0]) {
            s_current->force = true;
            s_current->force_why = TRANSIT_WHY_STOP;
        }
    }
    unlock();
}

void transit_session_refresh(void)
{
    if (!s_lock) return;
    lock();
    if (s_current) {
        s_current->force = true;
        s_current->force_why = TRANSIT_WHY_REFRESH;
    }
    unlock();
}

bool transit_session_take(uint32_t *seen, transit_status_t *status, transit_result_t *out)
{
    if (!s_lock) return false;
    bool got = false;
    lock();
    session_t *s = s_current;
    if (s && s->status.seq != *seen) {
        *seen = s->status.seq;
        *status = s->status;
        if (s->status.ok) memcpy(out, &s->result, sizeof *out);
        got = true;
    }
    unlock();
    return got;
}
