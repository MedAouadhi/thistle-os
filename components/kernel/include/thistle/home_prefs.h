// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stddef.h>
#include "esp_err.h"

/* The apps the user pinned to Home, place by place (home_prefs.rs), kept in
 * system.json as thistle_os.home.pins: a list of app IDs, "" for a place
 * never chosen. Home fills a place with no app installed from its own
 * choice. At most 8 places. */

/* The saved places, each ID NUL-terminated into `out + i * id_cap`, at most
 * `max`: returns how many there are, 0 when the user has pinned nothing. An
 * ID that does not fit is written as "". */
int rs_home_pins_get(char *out, size_t id_cap, int max);

/* Save `n` places, `ids[i]` NULL or "" for one not chosen. Whether it was
 * saved; ESP_ERR_INVALID_ARG for more than 8 places. */
esp_err_t rs_home_pins_set(const char *const *ids, int n);
