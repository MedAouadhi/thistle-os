// SPDX-License-Identifier: BSD-3-Clause
#pragma once

/*
 * The quick panel: switches and sliders that drop below the status bar when
 * the user drags down from it, or presses the quick key (HAL_KEY_QUICK).
 * Back, Esc, Home, a tap below it or a drag up close it.
 *
 * What it holds is registered, not built in: each switch is a
 * quick_toggle_t and each slider a quick_level_t, shown in the order they
 * were added and only where `available` says the board has the hardware.
 * The built-in ones are added by quick_panel_tiles.c, the same way a later
 * feature adds its own. The struct passed in is kept, not copied, so it must
 * outlive the panel: a static const.
 *
 * The callbacks run on the LVGL task, with the LVGL lock held. They must
 * return promptly; one that has to wait starts the work and returns, and the
 * panel shows the new state when is_on() reports it (it asks every second
 * while open).
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    const char *symbol;              /* drawn above the label: an LV_SYMBOL_ or a short word */
    const char *label;               /* a word or two */
    bool (*available)(void);         /* NULL: always */
    bool (*is_on)(void);
    /* Switch it. NULL when it worked; otherwise what to tell the user, in a
     * few words ("No modem on this board", "Not saved for next boot"). */
    const char *(*set_on)(bool on);
} quick_toggle_t;

typedef struct {
    const char *symbol;              /* drawn left of the slider */
    const char *label;               /* for logs and the keyboard's focus */
    bool (*available)(void);         /* NULL: always */
    uint8_t (*min)(void);            /* lowest level offered; NULL: 0 */
    uint8_t (*get)(void);            /* 0-100 */
    /* Set the level. `save` is false while the slider is being dragged and
     * true once it is let go, so a drag costs one write to flash. NULL when it
     * worked; otherwise what to tell the user. */
    const char *(*set)(uint8_t percent, bool save);
} quick_level_t;

#define QUICK_PANEL_MAX_TOGGLES 12
#define QUICK_PANEL_MAX_LEVELS   4

/* Add a switch or a slider. ESP_ERR_NO_MEM once the panel is full. */
esp_err_t quick_panel_add_toggle(const quick_toggle_t *toggle);
esp_err_t quick_panel_add_level(const quick_level_t *level);

/* Register the built-in switches and sliders (quick_panel_tiles.c). The UI
 * manager calls this once. */
void quick_panel_add_builtin(void);

/* Open, close, or the one it is not. LVGL task only. quick_panel_close()
 * says whether it was open. */
void quick_panel_open(void);
bool quick_panel_close(void);
void quick_panel_toggle(void);

/* Whether the panel is open. Safe from any task; a stale answer only costs a
 * gesture that is ignored. */
bool quick_panel_is_open(void);
