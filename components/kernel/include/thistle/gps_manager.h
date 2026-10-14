// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Whether the GPS receiver is being read (gps_manager.rs). It is started at
 * boot, so it is on unless switched off; the choice is kept in system.json as
 * thistle_os.gps.enabled and put back at boot. */

/* Whether the board has a GPS receiver. */
bool rs_gps_available(void);

bool rs_gps_is_enabled(void);

/* Switch GPS on or off and remember it. The driver's error if it would not
 * switch (ESP_ERR_NOT_SUPPORTED with no receiver); otherwise whether the
 * choice was saved. */
esp_err_t rs_gps_set_enabled(bool on);
