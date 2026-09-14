/*
 * ThistleOS — LVGL built-in app registration
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the LVGL-based built-in apps.
 *
 * Call this only when an LVGL window manager is running. Each of these apps
 * parents its widgets to ui_manager_get_app_area(), which is NULL until an
 * LVGL window manager has run ui_manager_init(). Registering them under
 * thistle-tk puts entries in the launcher that dereference NULL when opened.
 *
 * The list is shared by the firmware, the SDL simulator and the WASM
 * simulator so the three cannot drift: they once did, and the simulators
 * offered a launcher the device never shows.
 *
 * An app whose sources are absent from the build is compiled out through its
 * THISTLE_HAVE_* flag, which defaults to enabled when the build does not set
 * it. The two capability arguments gate the apps that need hardware.
 *
 * @param has_radio Board has a radio; gates Messenger and the WiFi scanner.
 * @param has_gps   Board has GNSS; gates Navigator.
 * @return ESP_OK, or the first error an app returned. Registration always
 *         runs to the end of the list.
 */
esp_err_t lvgl_apps_register_builtin(bool has_radio, bool has_gps);

#ifdef __cplusplus
}
#endif
