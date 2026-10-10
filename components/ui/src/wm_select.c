// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors

/*
 * wm_select.c — choose the window manager to bring up.
 *
 * Three window managers are compiled in and only two were ever reachable:
 * the display type picked between thistle-tk and the LVGL LCD manager, and
 * the LVGL e-paper manager was registered by nothing at all. system.json's
 * "window_manager" was read by board_config_get_wm_name() and then dropped,
 * so a board could not be asked for anything else.
 *
 * That mattered beyond tidiness. An LVGL app parents its widgets to
 * ui_manager_get_app_area(), which is NULL unless an LVGL manager ran
 * ui_manager_init(), so the LVGL apps are registered only when one is
 * running — and on an e-paper board none ever was. Twelve of the fourteen
 * built-in apps were unreachable on the T-Deck Pro and Max as a result.
 *
 * A board with no usable preference therefore gets an LVGL manager on either
 * display, so every built-in app is reachable by default. thistle-tk hosts
 * only its own launcher, App Store and Flashlight, and remains one name away
 * for a board that prefers its cheaper e-paper refresh to the other eleven.
 */

#include "ui/lvgl_wm.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "wm_select";

/* Anything that is not a manager's own `name`, including the "lvgl-wm" that
 * shipped in system.json and matches none of them. Spelled out so that a
 * preference which cannot be honoured is reported rather than ignored. */
static bool names_a_preference(const char *configured)
{
    return configured && *configured && strcmp(configured, "auto") != 0;
}

thistle_wm_choice_t thistle_wm_select(const char *configured, bool display_is_epaper)
{
    if (names_a_preference(configured)) {
        if (strcmp(configured, "thistle-tk") == 0) {
            return (thistle_wm_choice_t){ thistle_tk_wm_get(), false };
        }
        if (strcmp(configured, "lvgl-epaper-wm") == 0) {
            return (thistle_wm_choice_t){ lvgl_epaper_wm_get(), true };
        }
        if (strcmp(configured, "lvgl-lcd-wm") == 0) {
            return (thistle_wm_choice_t){ lvgl_lcd_wm_get(), true };
        }
        ESP_LOGW(TAG, "window_manager \"%s\" names no compiled manager; "
                      "using the default for this display", configured);
    }

    /* Both defaults host the LVGL built-ins. e-paper gets the manager that
     * defers and debounces its refreshes. */
    return display_is_epaper
               ? (thistle_wm_choice_t){ lvgl_epaper_wm_get(), true }
               : (thistle_wm_choice_t){ lvgl_lcd_wm_get(), true };
}
