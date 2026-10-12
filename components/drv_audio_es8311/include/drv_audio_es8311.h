// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — ES8311 audio codec driver header
#pragma once

#include "hal/audio.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Playback, and capture from the microphone on the codec's analog input when
 * the board wires the codec's ASDOUT back to the SoC. Without that line
 * (pin_din -1) the codec runs as a DAC alone and record() reports
 * ESP_ERR_NOT_SUPPORTED.
 */
typedef struct {
    int      i2s_num;           // I2S port number
    void    *i2c_bus;           // i2c_master_bus_handle_t the codec sits on
    uint8_t  i2c_addr;          // 7-bit address, 0x18 on the T-Deck Max
    int      pin_mclk;          // Master clock; the codec runs from MCLK
    int      pin_bclk;          // Bit clock
    int      pin_ws;            // Word select (LRCK)
    int      pin_dout;          // SoC data out, into the codec's DSDIN
    int      pin_din;           // SoC data in, from the codec's ASDOUT; -1 if none
    int32_t  amp_expander;      // IO expander index of the amp enable, -1 if none
    int32_t  amp_expander_pin;  // Pin on that expander, high = amplifier on
} audio_es8311_config_t;

/* Return the driver vtable instance. */
const hal_audio_driver_t *drv_audio_es8311_get(void);

#ifdef __cplusplus
}
#endif
