#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* OTA update source */
typedef enum {
    OTA_SOURCE_SD_CARD,     /* Update from /sdcard/update/thistle_os.bin */
    OTA_SOURCE_HTTP,        /* Update from HTTP URL */
} ota_source_t;

/* OTA progress callback */
typedef void (*ota_progress_cb_t)(uint32_t bytes_written, uint32_t total_bytes, void *user_data);

/* Initialize OTA subsystem */
esp_err_t ota_init(void);

/* Check if an OTA update file exists on SD card */
bool ota_sd_update_available(void);

/* Apply the firmware update on the SD card (/sdcard/update/thistle_os.bin and
 * its .sig): writes it into the other kernel slot (kernel_a <-> kernel_b,
 * never the reserved Recovery slot), selects it and restarts on success. */
esp_err_t ota_apply_from_sd(ota_progress_cb_t progress_cb, void *user_data);

/* Apply firmware update from HTTP URL
 * Downloads and writes to OTA partition, reboots on success. */
esp_err_t ota_apply_from_http(const char *url, ota_progress_cb_t progress_cb, void *user_data);

/* Get the currently running firmware version string */
const char *ota_get_current_version(void);

/* The running kernel slot's label: "kernel_a", "kernel_b", or "unknown" */
const char *ota_get_running_partition(void);

/* Keep a new kernel now, if the running one is new. Idempotent: once kept,
 * later calls do not invoke the OTA API again. */
esp_err_t ota_mark_valid(void);

/* Keep the running kernel once it has run for a minute, if it is new, then
 * call `then` (from the kernel worker); on any other boot call `then` at
 * once. Until it is kept, a reset rolls a new kernel back to the other
 * kernel slot. */
void ota_keep_new_kernel(void (*then)(void));

/* Remove the recovery bundle journal and backups after ota_mark_valid(). */
esp_err_t ota_finalize_bundle_transaction(void);

/* Rollback to previous firmware */
esp_err_t ota_rollback(void);
