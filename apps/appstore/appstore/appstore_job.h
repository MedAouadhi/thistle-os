/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — App Store worker
 *
 * Fetching the catalog and installing an entry take seconds to minutes of
 * network, so they run one at a time on a task of their own, never on
 * lvgl_task: its 8 KB stack held neither a TLS session nor the 100 KB of
 * catalog entries a refresh once put on it. A job outlives the App Store's
 * screens; a timer on lvgl_task hears it end, says how it went in a toast,
 * and tells the screens if they are open.
 */
#pragma once

#include "hal/sdcard_path.h"
#include "thistle/appstore_client.h"

#include <stdbool.h>

/* The catalog as the server last sent it (appstore_refresh_catalog). */
#define APPSTORE_CATALOG_PATH THISTLE_SDCARD "/appstore/catalog.json"

typedef enum {
    APPSTORE_JOB_REFRESH,    /* fetch the catalog and keep it on the card */
    APPSTORE_JOB_INSTALL,    /* download and install an app, driver or WM */
    APPSTORE_JOB_UPDATE_OS,  /* download firmware, apply it and restart */
} appstore_job_kind_t;

typedef struct {
    appstore_job_kind_t kind;
    esp_err_t err;
    const catalog_entry_t *entry;    /* INSTALL, UPDATE_OS: the one asked for */
    const catalog_entry_t *entries;  /* REFRESH: what came, count of them */
    int count;
} appstore_job_result_t;

/* Called on lvgl_task as a job ends; the result lasts for the call only. */
typedef void (*appstore_job_done_cb_t)(const appstore_job_result_t *result);

/* Start a job, copying the entry (NULL for REFRESH). False, with a toast
 * saying why, while another runs or without the memory or task for it. */
bool appstore_job_start(appstore_job_kind_t kind, const catalog_entry_t *entry);

bool appstore_job_busy(void);

/* The open App Store's callback; NULL as it closes. */
void appstore_job_set_done_cb(appstore_job_done_cb_t cb);
