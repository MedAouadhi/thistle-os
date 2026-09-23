// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — Bosch BHI260AP IMU driver header
//
// Implemented in components/kernel_rs/src/drv_imu_bhi260ap.rs.
#pragma once

#include "esp_err.h"
#include "hal/imu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the driver for a board profile entry, taking the entry's config
 * object as JSON: {"i2c_bus": 0, "i2c_addr": "0x28"}. Both keys are optional
 * and default to the values shown. */
esp_err_t drv_imu_bhi260ap_register(const char *config_json);

/* Return the driver vtable, configured with the defaults above. */
const hal_imu_driver_t *drv_imu_bhi260ap_get(void);

#ifdef __cplusplus
}
#endif
