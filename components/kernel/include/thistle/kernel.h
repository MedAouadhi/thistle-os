#pragma once

#include "esp_err.h"
#include <stdint.h>

#define THISTLE_VERSION_MAJOR 0
#define THISTLE_VERSION_MINOR 1
#define THISTLE_VERSION_PATCH 0
#define THISTLE_VERSION_STRING "0.1.0"

/* Initialize kernel subsystems (event bus, IPC, app manager, driver manager) */
esp_err_t kernel_init(void);

/* Register the thistle-tk App Store and Flashlight. Call only when thistle-tk
 * is the window manager: they share app IDs with the LVGL built-ins, and the
 * first registration of an ID is the one that is kept. */
esp_err_t tk_native_apps_register(void);

/* Get kernel uptime in milliseconds */
uint32_t kernel_uptime_ms(void);
