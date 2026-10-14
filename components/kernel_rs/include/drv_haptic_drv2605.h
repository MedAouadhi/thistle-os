/*
 * TI DRV2605 haptic driver: the vibration motor on the T-Deck Max.
 *
 * Implemented in Rust, in components/kernel_rs/src/drv_haptic_drv2605.rs.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the haptic HAL interface, taking the driver entry's config as the
 * JSON object a board profile declared:
 *
 *     {"i2c_bus": 0, "i2c_addr": "0x5A", "en_expander": 0, "en_expander_pin": 5}
 *
 * `i2c_bus` defaults to 0 and `i2c_addr` to 0x5A. `en_expander` and
 * `en_expander_pin` name the IO expander pin that powers the part, on the
 * T-Deck Max XL9555 P05 (`M_EN`); without them the part is taken to be
 * powered already.
 */
esp_err_t drv_haptic_drv2605_register(const char *config_json);

#ifdef __cplusplus
}
#endif
