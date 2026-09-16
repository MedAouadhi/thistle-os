/*
 * Stand-in window manager vtables for the unit test binary.
 *
 * thistle_wm_select() only ever returns one of three vtables; linking the real
 * ones would drag LVGL and the whole kernel into a test binary that exists to
 * stay small. These give it three distinguishable objects to choose between.
 *
 * The tests assert on identity rather than on `name`, so the selection logic
 * is what is under test and these strings cannot make a test pass by
 * agreeing with themselves. They carry the real names anyway, because the
 * names are what a user writes in system.json and seeing them here is worth
 * more than a placeholder: they mirror lvgl_wm_epaper.c, lvgl_wm_lcd.c and
 * kernel/src/tk_wm_shims.c.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "thistle/display_server.h"

static const display_server_wm_t s_tk         = { .name = "thistle-tk" };
static const display_server_wm_t s_lvgl_epd   = { .name = "lvgl-epaper-wm" };
static const display_server_wm_t s_lvgl_lcd   = { .name = "lvgl-lcd-wm" };

const display_server_wm_t *thistle_tk_wm_get(void)   { return &s_tk; }
const display_server_wm_t *lvgl_epaper_wm_get(void)  { return &s_lvgl_epd; }
const display_server_wm_t *lvgl_lcd_wm_get(void)     { return &s_lvgl_lcd; }
