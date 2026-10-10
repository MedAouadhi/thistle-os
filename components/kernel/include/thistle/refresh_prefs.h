// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdint.h>
#include "esp_err.h"

/* The e-paper refresh policy saved in system.json, as
 * thistle_os.display.refresh_mode — one of epaper_refresh_policy_t's values
 * (ui/epaper_refresh.h). Fast if none is saved. */
uint8_t refresh_prefs_load(void);

/* Save the refresh policy so the UI manager applies it at the next boot. */
esp_err_t refresh_prefs_save(uint8_t policy);
