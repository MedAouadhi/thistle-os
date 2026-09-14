// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — GDEQ031T10 e-paper display driver header
#pragma once

#include "hal/display.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t spi_host;
    gpio_num_t        pin_cs;
    gpio_num_t        pin_dc;
    gpio_num_t        pin_rst;
    gpio_num_t        pin_busy;
    /* Front-light PWM. GPIO_NUM_NC on panels lit only by ambient light —
     * the T-Deck Pro has no front-light, the T-Deck Max does. */
    gpio_num_t        pin_bl;
    int               spi_clock_hz;
    /* How the panel is mounted, in degrees: 0 or 180. Boards carrying the same
     * panel do not orient it the same way — the T-Deck Max mounts it turned
     * around from the T-Deck Pro — and the controller can scan either way, so
     * this costs nothing at run time. Anything but 180 is treated as 0. */
    int               rotation;
} epaper_gdeq031t10_config_t;

/* Return the driver vtable instance. */
const hal_display_driver_t *drv_epaper_gdeq031t10_get(void);

#ifdef __cplusplus
}
#endif
