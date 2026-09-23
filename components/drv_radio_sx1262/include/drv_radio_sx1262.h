// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — SX1262 LoRa radio driver header
#pragma once

#include "hal/radio.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t spi_host;
    gpio_num_t        pin_cs;
    gpio_num_t        pin_reset;
    gpio_num_t        pin_busy;
    gpio_num_t        pin_dio1;     // Interrupt line
    int               spi_clock_hz;
    // The band the board is matched and licensed for. The same part ships at
    // 868 MHz in Europe and 915 MHz in North America, so this belongs to the
    // board, not the driver. Zero means the 915 MHz this driver has always
    // begun at, which is what a board declaring nothing keeps.
    uint32_t          freq_hz;
    // The reference voltage the module's DIO3 supplies to an on-board TCXO,
    // in millivolts. Zero means the module runs a plain crystal and must not
    // be sent the command at all. A module fitted with a TCXO that never gets
    // one still reports a successful begin(), and then times out on every
    // transmission waiting for a TxDone its unlocked PLL cannot produce.
    uint16_t          tcxo_mv;
    // Whether DIO2 drives the module's own RF switch between its transmit and
    // receive paths. This is separate from any switch the board puts in front
    // of the module to choose between antennas.
    bool              dio2_rf_switch;
} radio_sx1262_config_t;

/* Return the driver vtable instance. */
const hal_radio_driver_t *drv_radio_sx1262_get(void);

#ifdef __cplusplus
}
#endif
