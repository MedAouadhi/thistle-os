/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Haptics HAL interface
 *
 * A vibration motor, and whatever drives it. Callers ask for what the user
 * should feel, not for a waveform: a driver for a motor on a plain GPIO
 * times pulses itself, one for a haptic driver chip picks from its library.
 */
#pragma once

#include "esp_err.h"

typedef enum {
    HAL_HAPTIC_CLICK,     /* one short, firm click */
    HAL_HAPTIC_MESSAGE,   /* a message arrived: two buzzes */
} hal_haptic_effect_t;

typedef struct {
    esp_err_t (*init)(const void *config);
    void (*deinit)(void);
    /* Start `effect` and return without waiting for it to end. Any task may
     * call it, never an interrupt handler. One asked for while another plays
     * follows it. */
    esp_err_t (*play)(hal_haptic_effect_t effect);
    const char *name;
} hal_haptic_driver_t;
