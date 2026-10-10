// SPDX-License-Identifier: BSD-3-Clause
#pragma once

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
