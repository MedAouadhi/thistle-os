#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    HAL_POWER_STATE_DISCHARGING,
    HAL_POWER_STATE_CHARGING,
    HAL_POWER_STATE_CHARGED,
    HAL_POWER_STATE_NO_BATTERY,
} hal_power_state_t;

typedef struct {
    uint16_t voltage_mv;
    uint8_t percent;
    hal_power_state_t state;
} hal_power_info_t;

typedef struct {
    esp_err_t (*init)(const void *config);
    void (*deinit)(void);
    esp_err_t (*get_info)(hal_power_info_t *info);
    uint16_t (*get_battery_mv)(void);
    uint8_t (*get_battery_percent)(void);
    bool (*is_charging)(void);
    esp_err_t (*sleep)(bool enter);
    const char *name;
    /* Whether USB or an adapter powers the board, charging or not: a full
     * battery whose charging has paused is still plugged in. NULL when the
     * driver cannot tell; the kernel then takes charging or charged for
     * plugged in. Last, so that no field before it moved. */
    bool (*is_plugged_in)(void);
} hal_power_driver_t;
