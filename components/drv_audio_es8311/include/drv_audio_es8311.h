// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — ES8311 audio codec driver header
#pragma once

#include "hal/audio.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Playback only. The codec's ADC is left unconfigured: the audio HAL has no
 * capture path, so its microphone input is unreachable.
 *
 * TODO: audio capture needs a HAL path before the microphone can be used —
 * the same shape of gap as haptics, a kernel ABI change of its own.
 */
typedef struct {
    int      i2s_num;           // I2S port number
    void    *i2c_bus;           // i2c_master_bus_handle_t the codec sits on
    uint8_t  i2c_addr;          // 7-bit address, 0x18 on the T-Deck Max
    int      pin_mclk;          // Master clock; the codec runs from MCLK
    int      pin_bclk;          // Bit clock
    int      pin_ws;            // Word select (LRCK)
    int      pin_dout;          // SoC data out, into the codec's DSDIN
    int32_t  amp_expander;      // IO expander index of the amp enable, -1 if none
    int32_t  amp_expander_pin;  // Pin on that expander, high = amplifier on
} audio_es8311_config_t;

/* Return the driver vtable instance. */
const hal_audio_driver_t *drv_audio_es8311_get(void);

#ifdef __cplusplus
}
#endif
