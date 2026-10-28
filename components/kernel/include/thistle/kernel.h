#pragma once

#include "esp_err.h"
#include <stdint.h>

/* The firmware's version is its tag, described at build time
 * (tools/firmware_version.sh): ota_get_current_version() (thistle/ota.h)
 * gives it, "0.6.0" or, built on a laptop, "0.5.2-108-gabc1234-dirty". */

/* Initialize kernel subsystems (event bus, IPC, app manager, driver manager) */
esp_err_t kernel_init(void);

/* Register the thistle-tk App Store and Flashlight. Call only when thistle-tk
 * is the window manager: they share app IDs with the LVGL built-ins, and the
 * first registration of an ID is the one that is kept. */
esp_err_t tk_native_apps_register(void);

/* Get kernel uptime in milliseconds */
uint32_t kernel_uptime_ms(void);
