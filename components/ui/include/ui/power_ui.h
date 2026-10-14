/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the sleep screen and the power menu, under the LVGL window
 * managers. The power manager decides; this shows what it asks for.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* Start answering the power manager's requests. ui_manager_init() calls this
 * once LVGL is up; from then on, holding the power key opens the menu. */
esp_err_t power_ui_init(void);

/* Whether the sleep screen is up, drawn into the framebuffer. The panel may
 * still be refreshing it. LVGL context only. */
bool power_ui_sleep_screen_shown(void);

/* Close the power menu, if it is open; whether it was. LVGL context only. */
bool power_ui_close_menu(void);
