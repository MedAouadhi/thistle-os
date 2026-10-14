// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS Simulator — fake vibration motor, which prints what it plays

#include "sim_haptic.h"

#include <stdio.h>

static esp_err_t sim_haptic_init(const void *config)
{
    (void)config;
    return ESP_OK;
}

static void sim_haptic_deinit(void)
{
}

static esp_err_t sim_haptic_play(hal_haptic_effect_t effect)
{
    switch (effect) {
        case HAL_HAPTIC_CLICK:   printf("[sim_haptic] click\n"); break;
        case HAL_HAPTIC_MESSAGE: printf("[sim_haptic] message buzz\n"); break;
        default:                 return ESP_ERR_INVALID_ARG;
    }
    fflush(stdout);
    return ESP_OK;
}

static const hal_haptic_driver_t sim_haptic_driver = {
    .init   = sim_haptic_init,
    .deinit = sim_haptic_deinit,
    .play   = sim_haptic_play,
    .name   = "Simulator vibration motor",
};

const hal_haptic_driver_t *sim_haptic_get(void)
{
    return &sim_haptic_driver;
}
