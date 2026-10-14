#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* One format for both directions: a codec that plays and records shares one
 * word clock, so the rate set here is the rate of each. */
typedef struct {
    uint32_t sample_rate;
    uint8_t bits_per_sample;
    uint8_t channels;
} hal_audio_config_t;

/* What the board's speaker and headphone output carries, on a board whose
 * audio switch can hand it to another source. */
typedef enum {
    HAL_AUDIO_ROUTE_CODEC = 0,  /* this driver's own output, as at init */
    HAL_AUDIO_ROUTE_VOICE = 1,  /* the voice channel of a modem wired to the switch */
} hal_audio_route_t;

typedef struct {
    esp_err_t (*init)(const void *config);
    void (*deinit)(void);
    esp_err_t (*play)(const uint8_t *data, size_t len);
    /* End playback and capture, and power the output down. */
    esp_err_t (*stop)(void);
    esp_err_t (*set_volume)(uint8_t percent);
    esp_err_t (*configure)(const hal_audio_config_t *cfg);
    const char *name;
    /* Fill buf with len bytes from the microphone, in the configured format,
     * blocking until they have arrived. NULL on a device with no audio input,
     * and ESP_ERR_NOT_SUPPORTED on one whose board left the input unwired. */
    esp_err_t (*record)(uint8_t *buf, size_t len);
    /* Microphone gain in dB, rounded down to what the codec can do. NULL
     * where record is. */
    esp_err_t (*set_mic_gain)(uint8_t db);
    /* Hand the speaker and headphone output to `route`. For
     * HAL_AUDIO_ROUTE_VOICE the speaker amplifier is on if `speaker`, off if
     * not (headphones only), and stays as set until the route changes:
     * playback here is stopped, and play() refused, meanwhile. Back to
     * HAL_AUDIO_ROUTE_CODEC, the output is this driver's again and the
     * amplifier follows playback as before. Optional: NULL on a board with
     * no audio switch. Appended last, so no field above moves. */
    esp_err_t (*set_route)(hal_audio_route_t route, bool speaker);
} hal_audio_driver_t;
