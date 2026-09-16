// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — Hynitron CST66xx capacitive touch driver header
#pragma once

#include "hal/input.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    uint8_t                 i2c_addr;         /* Default 0x1A */
    gpio_num_t              pin_int;          /* Interrupt pin. GPIO_NUM_NC to disable. */
    gpio_num_t              pin_rst;          /* Reset pin, when it is a GPIO at all. */
    int32_t                 rst_expander;     /* IO expander index, -1 if none */
    int32_t                 rst_expander_pin; /* Pin on that expander */
    uint16_t                max_x;            /* Panel width  (e.g. 240) */
    uint16_t                max_y;            /* Panel height (e.g. 320) */
} touch_cst66xx_config_t;

/* Return the driver vtable instance. */
const hal_input_driver_t *drv_touch_cst66xx_get(void);

#ifdef __cplusplus
}
#endif
