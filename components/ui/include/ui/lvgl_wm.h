// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "thistle/display_server.h"

/* Get the LVGL e-paper window manager vtable.
 * Uses deferred refresh with debounce, no splash screen. */
const display_server_wm_t *lvgl_epaper_wm_get(void);

/* Get the LVGL LCD window manager vtable.
 * Uses direct flush, shows splash screen on init. */
const display_server_wm_t *lvgl_lcd_wm_get(void);

/* Get the thistle-tk (pure Rust) window manager vtable.
 * Uses embedded-graphics renderer. Works with both e-paper and LCD.
 * Defined in kernel/src/tk_wm_shims.c, backed by kernel_rs/src/tk_wm.rs. */
const display_server_wm_t *thistle_tk_wm_get(void);

/* A chosen window manager, and whether it can host the LVGL built-in apps.
 *
 * The two travel together because registering those apps under a manager that
 * cannot host them is not a cosmetic mistake: they parent their widgets to
 * ui_manager_get_app_area(), NULL unless an LVGL manager ran ui_manager_init(),
 * so launching one segfaults. Callers decide both from this one answer. */
typedef struct {
    const display_server_wm_t *wm;
    bool                       hosts_lvgl_apps;
} thistle_wm_choice_t;

/* Pick a window manager from system.json's "window_manager", falling back to
 * the board's display type when it names nothing usable.
 *
 * `configured` may be NULL, empty or "auto" for the fallback; pass
 * board_config_get_wm_name(). Valid names are each manager's own `name`:
 * "thistle-tk", "lvgl-epaper-wm", "lvgl-lcd-wm". */
thistle_wm_choice_t thistle_wm_select(const char *configured, bool display_is_epaper);
