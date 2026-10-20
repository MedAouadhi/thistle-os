#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    double latitude;
    double longitude;
    float altitude_m;
    float speed_kmh;
    float heading_deg;
    uint8_t satellites;
    bool fix_valid;
    uint32_t timestamp;   // UTC time as unix timestamp
} hal_gps_position_t;

/* What the receiver hears, fix or not: what tells a receiver that needs a
 * better view of the sky from one that hears nothing at all. */
typedef struct {
    uint8_t in_view;    // satellites it lists above the horizon
    uint8_t heard;      // of those, how many it receives
    uint8_t best_cno;   // the strongest signal, dB-Hz; 0 with none heard
} hal_gps_sky_t;

typedef void (*hal_gps_cb_t)(const hal_gps_position_t *pos, void *user_data);

typedef struct {
    esp_err_t (*init)(const void *config);
    void (*deinit)(void);
    esp_err_t (*enable)(void);
    esp_err_t (*disable)(void);
    /* ESP_OK whenever the receiver is being read, with or without a fix:
     * fix_valid says which, and satellites is meaningful either way.
     * An error means nothing is reading the receiver. */
    esp_err_t (*get_position)(hal_gps_position_t *pos);
    esp_err_t (*register_callback)(hal_gps_cb_t cb, void *user_data);
    esp_err_t (*sleep)(bool enter);
    const char *name;
    /* The last few seconds' sky, with the same ESP_OK as get_position.
     * NULL when the driver cannot tell. Last, so that no field before it
     * moved. */
    esp_err_t (*get_sky)(hal_gps_sky_t *sky);
} hal_gps_driver_t;
