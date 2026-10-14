#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    HAL_INPUT_EVENT_KEY_DOWN,
    HAL_INPUT_EVENT_KEY_UP,
    HAL_INPUT_EVENT_TOUCH_DOWN,
    HAL_INPUT_EVENT_TOUCH_UP,
    HAL_INPUT_EVENT_TOUCH_MOVE,
} hal_input_event_type_t;

typedef struct {
    hal_input_event_type_t type;
    uint32_t timestamp;
    union {
        struct { uint16_t keycode; } key;
        struct { uint16_t x; uint16_t y; } touch;
    };
} hal_input_event_t;

/* Keycodes are ASCII for keys that type something. Codes past the 8-bit range
 * name keys that type nothing; the window manager acts on them, and they are
 * never delivered to an app as text. */
#define HAL_KEY_HOME 0x0100  /* Back to the launcher's home screen */
#define HAL_KEY_BACK 0x0101  /* Up one page in the app in front; Home from its first */
#define HAL_KEY_QUICK 0x0102 /* Open or close the quick panel */

typedef void (*hal_input_cb_t)(const hal_input_event_t *event, void *user_data);

typedef struct {
    esp_err_t (*init)(const void *config);
    void (*deinit)(void);
    esp_err_t (*register_callback)(hal_input_cb_t cb, void *user_data);
    esp_err_t (*poll)(void);
    const char *name;
    bool is_touch;
    /* Set the key backlight, 0-100 percent. NULL on a device whose keys are
     * not lit, and ESP_ERR_NOT_SUPPORTED on one where the board left the
     * backlight pin unwired. Mirrors set_brightness on the display. */
    esp_err_t (*set_backlight)(uint8_t percent);
    /* Optional, for the power manager. enter: stop reporting and drop to the
     * part's lowest power, leaving it able to wake the device if it is a
     * wake source: a keyboard's interrupt line idle, so the next key pressed
     * pulls it. !enter: report again, dropping whatever woke the device, so
     * the key pressed to wake it is not typed. NULL where there is nothing
     * to do. */
    esp_err_t (*sleep)(bool enter);
} hal_input_driver_t;
