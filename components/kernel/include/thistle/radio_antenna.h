// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Which antenna the LoRa radio uses, on a board whose profile declares a
 * radio_antenna_switch. Mirrors the constants in radio_antenna.rs. */
typedef enum {
    RADIO_ANTENNA_INTERNAL = 0,
    RADIO_ANTENNA_EXTERNAL = 1,
} radio_antenna_t;

/* Whether this board has an antenna to choose. */
bool radio_antenna_switchable(void);

/* The antenna the radio is routed to now. */
uint8_t radio_antenna_get(void);

/* Route the radio to an antenna now, without saving the choice.
 * ESP_ERR_NOT_SUPPORTED on a board with no switch. Nothing can tell whether
 * an antenna is screwed on: choosing external is the caller's word that one is. */
esp_err_t radio_antenna_select(uint8_t antenna);

/* Save the choice to system.json, under thistle_os.radio.antenna, for the
 * next boot. */
esp_err_t radio_antenna_save(uint8_t antenna);

/* Route the radio to the saved antenna. kernel_init() calls this once the
 * drivers have started. */
void radio_antenna_restore(void);
