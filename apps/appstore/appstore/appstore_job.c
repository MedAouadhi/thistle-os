/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — App Store worker (see appstore_job.h)
 */
#include "appstore/appstore_job.h"

#include "thistle/power_manager.h"
#include "ui/toast.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "appstore_job";

/* TLS wants 8 KB, as Dictation's task and the Podcasts threads have. An
 * install's deepest point is later, verifying the package's ELF: about 2 KB
 * of install_tap_with() and its callers, then Ed25519's verification, 7 to
 * 8 KB (vartime_double_scalar_mul 3.5 KB, its NAF table 2 KB, RCompute 1 KB
 * and the field arithmetic). With extraction's 4 KB buffer on the stack too
 * it overflowed 12 KB on a T-Deck Max. The task lives only while a job runs. */
#define TASK_STACK     16384
#define TASK_PRIORITY  4
#define POLL_MS        250

typedef struct {
    appstore_job_kind_t kind;
    catalog_entry_t entry;
    catalog_entry_t *entries;   /* REFRESH: CATALOG_MAX_ENTRIES of them */
    int count;
    esp_err_t err;
    atomic_bool done;
} job_t;

static struct {
    job_t *job;
    lv_timer_t *timer;
    uint32_t hold;
    appstore_job_done_cb_t done_cb;
} s;

/* Both the job and a refresh's 100 KB of entries go to PSRAM, which only
 * this task and lvgl_task touch. */
static void *psram_calloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (p) memset(p, 0, size);
    return p;
}

static void job_free(job_t *job)
{
    if (!job) return;
    free(job->entries);
    free(job);
}

/* ------------------------------------------------------------------ */
/* The task                                                             */
/* ------------------------------------------------------------------ */

static void job_run(job_t *job)
{
    switch (job->kind) {
    case APPSTORE_JOB_REFRESH:
        job->err = appstore_refresh_catalog(NULL, APPSTORE_CATALOG_PATH, job->entries,
                                            CATALOG_MAX_ENTRIES, &job->count);
        break;
    case APPSTORE_JOB_INSTALL:
        job->err = appstore_install_entry(&job->entry, NULL, NULL);
        break;
    }
}

static void job_task(void *arg)
{
    job_t *job = arg;
    job_run(job);
#ifndef SIMULATOR_BUILD
    ESP_LOGI(TAG, "done: %s (0x%x); %u bytes of stack to spare", esp_err_to_name(job->err),
             (unsigned)job->err, (unsigned)uxTaskGetStackHighWaterMark(NULL));
#else
    ESP_LOGI(TAG, "done: %s (0x%x)", esp_err_to_name(job->err), (unsigned)job->err);
#endif
    atomic_store(&job->done, true);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* The LVGL side                                                        */
/* ------------------------------------------------------------------ */

static void say_result(const job_t *job)
{
    char msg[128];
    const char *name = job->entry.name[0] ? job->entry.name : job->entry.id;

    switch (job->kind) {
    case APPSTORE_JOB_REFRESH:
        if (job->err == ESP_OK && job->count > 0) {
            toast_show("Catalog updated", TOAST_SUCCESS, 2000);
        } else if (job->err == ESP_OK) {
            toast_warn("The catalog lists nothing for this device");
        } else if (job->err == ESP_ERR_INVALID_SIZE) {
            toast_warn("The catalog is too large to read");
        } else {
            toast_warn("Could not reach the catalog server");
        }
        return;
    case APPSTORE_JOB_INSTALL:
        if (job->err == ESP_OK) {
            snprintf(msg, sizeof msg, "%s installed. Restart to %s it.", name,
                     job->entry.type == CATALOG_TYPE_APP ? "open" : "use");
            toast_show(msg, TOAST_SUCCESS, 4000);
            return;
        }
        break;
    }

    switch (job->err) {
    case ESP_ERR_INVALID_CRC:
        snprintf(msg, sizeof msg, "%s: corrupted, or its signature is invalid", name);
        break;
    case ESP_ERR_INVALID_ARG:
        snprintf(msg, sizeof msg, "This catalog has no installable package of %s", name);
        break;
    case ESP_ERR_INVALID_SIZE:
        snprintf(msg, sizeof msg, "%s: the download did not match the catalog", name);
        break;
    case ESP_ERR_NOT_SUPPORTED:
        snprintf(msg, sizeof msg, "A newer %s is already installed", name);
        break;
    default:
        snprintf(msg, sizeof msg, "%s: download failed", name);
        break;
    }
    toast_warn(msg);
}

static void poll_cb(lv_timer_t *timer)
{
    job_t *job = s.job;
    if (job && !atomic_load(&job->done)) return;

    /* Ended: let another start, even from the callback below. */
    s.job = NULL;
    if (s.timer == timer) s.timer = NULL;
    lv_timer_delete(timer);
    if (s.hold) power_manager_release(s.hold);
    s.hold = 0;
    if (!job) return;

    say_result(job);
    if (s.done_cb) {
        appstore_job_result_t result = {
            .kind = job->kind,
            .err = job->err,
            .entry = &job->entry,
            .entries = job->entries,
            .count = job->count,
        };
        s.done_cb(&result);
    }
    job_free(job);
}

bool appstore_job_start(appstore_job_kind_t kind, const catalog_entry_t *entry)
{
    if (s.job) {
        toast_warn("The App Store is busy; try again when it is done");
        return false;
    }

    job_t *job = psram_calloc(sizeof(*job));
    if (job && kind == APPSTORE_JOB_REFRESH) {
        job->entries = psram_calloc(sizeof(catalog_entry_t) * CATALOG_MAX_ENTRIES);
    }
    if (!job || (kind == APPSTORE_JOB_REFRESH && !job->entries) || (kind != APPSTORE_JOB_REFRESH && !entry)) {
        job_free(job);
        toast_warn("Not enough memory for the App Store");
        return false;
    }
    job->kind = kind;
    if (entry) job->entry = *entry;
    atomic_init(&job->done, false);

    s.hold = power_manager_hold(kind == APPSTORE_JOB_REFRESH ? "fetching the app catalog"
                                                             : "installing from the App Store");
#ifdef __EMSCRIPTEN__
    /* The browser build's xTaskCreate runs nothing: the job runs here. */
    job_run(job);
    atomic_store(&job->done, true);
#else
    if (xTaskCreate(job_task, "appstore", TASK_STACK, job, TASK_PRIORITY, NULL) != pdPASS) {
        power_manager_release(s.hold);
        s.hold = 0;
        job_free(job);
        toast_warn("Could not start the download");
        return false;
    }
#endif
    /* The task cannot end unheard: poll_cb runs on this task, lvgl_task. */
    s.job = job;
    s.timer = lv_timer_create(poll_cb, POLL_MS, NULL);
    return true;
}

bool appstore_job_busy(void)
{
    return s.job != NULL;
}

void appstore_job_set_done_cb(appstore_job_done_cb_t cb)
{
    s.done_cb = cb;
}
