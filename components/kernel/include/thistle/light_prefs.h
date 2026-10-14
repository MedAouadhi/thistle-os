// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Save the screen's light level (0-100 percent) to system.json, under
 * thistle_os.display.light, so it is restored at the next boot. */
esp_err_t light_prefs_save_display(uint8_t percent);

/* Save the key backlight level (0-100 percent) to system.json, under
 * thistle_os.keyboard.light. */
esp_err_t light_prefs_save_keys(uint8_t percent);

/* Apply the saved levels to the registered display and input drivers.
 * kernel_init() calls this once the drivers have started. */
void light_prefs_restore(void);

/* The lights as they are now (light_prefs.rs). Set them through these, not
 * the drivers, so that what they are at is known. A light is taken to be
 * there until its driver answers ESP_ERR_NOT_SUPPORTED, which boot asks
 * without lighting anything. */

bool light_display_available(void);
uint8_t light_display_level(void);
/* The dimmest the screen should be offered: above 0 on a screen that shows
 * nothing without its light. */
uint8_t light_display_min(void);
/* Set the screen's light, and save the level if `save`. The driver's error if
 * it refused; otherwise whether the level was saved. */
esp_err_t light_set_display(uint8_t percent, bool save);

bool light_keys_available(void);
uint8_t light_keys_level(void);
/* The level the keys go back to when switched on: the last they were lit at. */
uint8_t light_keys_on_level(void);
esp_err_t light_set_keys(uint8_t percent, bool save);
