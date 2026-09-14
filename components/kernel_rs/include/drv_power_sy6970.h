/*
 * SY6970 / BQ25896 charger + BQ27220 fuel gauge power driver.
 *
 * Implemented in Rust, in components/kernel_rs/src/drv_power_sy6970.rs.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the power HAL interface for this board's charger and fuel gauge.
 *
 * Unlike the drivers that take a config struct, this one reads its own driver
 * entry, given here as the JSON object a board profile declared:
 *
 *     {"i2c_bus": 0, "gauge_i2c_addr": "0x55"}
 *
 * `i2c_bus` is a bus index and defaults to 0; `gauge_i2c_addr` defaults to
 * 0x55 and exists for a board that wires the gauge elsewhere. The charger's
 * address is deliberately absent: a board cannot know which of the two
 * register-compatible parts its hardware revision shipped with, so the driver
 * probes both addresses and identifies the part it finds.
 *
 * Reading the entry here rather than in `init` is what lets the board profile
 * path pass a temporary: the HAL registry stores a config pointer and calls
 * `init` much later, from the driver manager.
 *
 * @param config_json The driver entry's config object, or NULL for defaults.
 * @return ESP_OK when the interface is registered.
 */
esp_err_t drv_power_sy6970_register(const char *config_json);

#ifdef __cplusplus
}
#endif
