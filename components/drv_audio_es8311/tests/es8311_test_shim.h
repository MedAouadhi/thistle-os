// SPDX-License-Identifier: BSD-3-Clause
//
// Host-test shim for drv_audio_es8311.c. The driver's hardware port — the
// codec, the idle timer, its mutex and the expander setter — is replaced by
// fakes that record what the driver asked for, in order. Everything above the
// port runs unchanged.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define esp_err_to_name(e) "err"

// ── Event log ────────────────────────────────────────────────────────────────

typedef enum {
    EV_CODEC_CREATE,
    EV_CODEC_OPEN,
    EV_CODEC_WRITE,
    EV_CODEC_CLOSE,
    EV_CODEC_DESTROY,
    EV_CODEC_VOLUME,
    EV_CODEC_READ,
    EV_CODEC_MIC_GAIN,
    EV_AMP_ON,
    EV_AMP_OFF,
} shim_event_kind_t;

typedef struct {
    shim_event_kind_t kind;
    uint32_t          arg;
} shim_event_t;

static shim_event_t s_events[64];
static size_t       s_event_count;

static void shim_record(shim_event_kind_t kind, uint32_t arg)
{
    if (s_event_count < sizeof(s_events) / sizeof(s_events[0])) {
        s_events[s_event_count++] = (shim_event_t){ kind, arg };
    }
}

// ── Failure injection and hooks ──────────────────────────────────────────────

static esp_err_t s_codec_create_result;
static esp_err_t s_expander_result;
static void    (*s_during_write)(void);
static void    (*s_during_read)(void);

// ── IO expander setter ───────────────────────────────────────────────────────

static int s_expander_calls;
static int s_last_expander;
static int s_last_expander_pin;

int thistle_expander_set_level(int expander, int pin, int level)
{
    s_expander_calls++;
    s_last_expander     = expander;
    s_last_expander_pin = pin;
    if (s_expander_result != ESP_OK) return s_expander_result;
    shim_record(level ? EV_AMP_ON : EV_AMP_OFF, (uint32_t)pin);
    return ESP_OK;
}

// ── Codec port ───────────────────────────────────────────────────────────────

static esp_err_t port_codec_create(const audio_es8311_config_t *cfg)
{
    (void)cfg;
    if (s_codec_create_result != ESP_OK) return s_codec_create_result;
    shim_record(EV_CODEC_CREATE, 0);
    return ESP_OK;
}

static void port_codec_destroy(void) { shim_record(EV_CODEC_DESTROY, 0); }

static esp_err_t port_codec_open(const hal_audio_config_t *fs)
{
    shim_record(EV_CODEC_OPEN, fs->sample_rate);
    return ESP_OK;
}

static void port_codec_close(void) { shim_record(EV_CODEC_CLOSE, 0); }

static esp_err_t port_codec_write(const uint8_t *data, size_t len)
{
    (void)data;
    shim_record(EV_CODEC_WRITE, (uint32_t)len);
    if (s_during_write) s_during_write();
    return ESP_OK;
}

static esp_err_t port_codec_read(uint8_t *buf, size_t len)
{
    memset(buf, 0, len);
    shim_record(EV_CODEC_READ, (uint32_t)len);
    if (s_during_read) s_during_read();
    return ESP_OK;
}

static esp_err_t port_codec_set_volume(uint8_t percent)
{
    shim_record(EV_CODEC_VOLUME, percent);
    return ESP_OK;
}

static esp_err_t port_codec_set_mic_gain(uint8_t db)
{
    shim_record(EV_CODEC_MIC_GAIN, db);
    return ESP_OK;
}

// ── Clock and heap ───────────────────────────────────────────────────────────

static int64_t s_now_us;
static int64_t esp_timer_get_time(void) { return s_now_us; }

#define MALLOC_CAP_SPIRAM  (1 << 10)
#define MALLOC_CAP_DEFAULT (1 << 12)
static void *heap_caps_malloc_prefer(size_t size, size_t num, ...)
{
    (void)num;
    return malloc(size);
}
static void heap_caps_free(void *p) { free(p); }

// ── Mutex ────────────────────────────────────────────────────────────────────

typedef int *SemaphoreHandle_t;
typedef uint32_t TickType_t;
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1

// One cell per mutex the driver creates; non-zero while held.
static int s_mutex_pool[4];
static int s_mutex_count;

static SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    if (s_mutex_count == (int)(sizeof(s_mutex_pool) / sizeof(s_mutex_pool[0]))) return NULL;
    s_mutex_pool[s_mutex_count] = 0;
    return &s_mutex_pool[s_mutex_count++];
}

// Single-threaded: a take that would block is a take that fails, which is
// exactly what a zero-timeout take from the timer sees while play holds it.
static int xSemaphoreTake(SemaphoreHandle_t m, TickType_t timeout)
{
    (void)timeout;
    if (*m) return 0;
    *m = 1;
    return pdTRUE;
}

static int xSemaphoreGive(SemaphoreHandle_t m)
{
    *m = 0;
    return pdTRUE;
}


// ── Idle timer ───────────────────────────────────────────────────────────────

typedef struct shim_timer *esp_timer_handle_t;
typedef struct {
    void      (*callback)(void *arg);
    void       *arg;
    const char *name;
} esp_timer_create_args_t;

struct shim_timer {
    void   (*callback)(void *arg);
    void    *arg;
    bool     armed;
    uint64_t timeout_us;
};

static struct shim_timer s_timer;

static esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                                  esp_timer_handle_t *out)
{
    s_timer = (struct shim_timer){ args->callback, args->arg, false, 0 };
    *out = &s_timer;
    return ESP_OK;
}

static esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us)
{
    t->armed = true;
    t->timeout_us = timeout_us;
    return ESP_OK;
}

static esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    t->armed = false;
    return ESP_OK;
}

// Let the idle timeout elapse. Returns false if nothing had armed it.
static bool shim_fire_idle_timer(void)
{
    if (!s_timer.armed) return false;
    s_timer.armed = false;
    s_timer.callback(s_timer.arg);
    return true;
}
