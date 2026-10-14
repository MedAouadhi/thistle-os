// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — NXP PCF8563 real-time clock (drv_rtc_pcf8563.rs)
#pragma once

#include "esp_err.h"
#include "hal/rtc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The driver's vtable. */
const hal_rtc_driver_t *drv_rtc_pcf8563_get(void);

/**
 * Bring the chip up and register it as the RTC, taking the driver entry's
 * config as the JSON object a board profile declared:
 *
 *     {"i2c_bus": 0, "i2c_addr": "0x51"}
 *
 * Both default to those values; 0x51 is the part's only address.
 */
esp_err_t drv_rtc_pcf8563_register(const char *config_json);

#ifdef __cplusplus
}
#endif
