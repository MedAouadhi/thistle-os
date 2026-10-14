/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 ThistleOS Contributors
 */
#include "sim_audio.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

/* The format set, so play() can take as long as the samples would. */
static uint32_t s_rate = 44100;
static uint8_t  s_bits = 16;
static uint8_t  s_channels = 2;
static size_t   s_played;     /* bytes since the format was set, for the log */
/* What the speaker carries: this driver's output, or a call's voice, during
 * which play() is refused as on the device. */
static atomic_int s_route = HAL_AUDIO_ROUTE_CODEC;

static esp_err_t sim_audio_init(const void *config)
{
    (void)config;
    printf("[sim_audio] init\n");
    return ESP_OK;
}

static void sim_audio_deinit(void)
{
    printf("[sim_audio] deinit\n");
}

/* No host audio is opened: the samples are thrown away. As on the device,
 * where play() blocks until the DMA has taken them, it returns after as long
 * as they would take to play, so a player paces itself as it would there. */
static esp_err_t sim_audio_play(const uint8_t *data, size_t len)
{
    (void)data;
    if (!len) return ESP_ERR_INVALID_ARG;
    if (atomic_load(&s_route) == HAL_AUDIO_ROUTE_VOICE) return ESP_ERR_INVALID_STATE;
    if (s_played == 0) printf("[sim_audio] playing\n");
    s_played += len;
    size_t frame = (size_t)s_channels * (s_bits / 8);
    if (frame && s_rate) {
        unsigned long long us = (unsigned long long)(len / frame) * 1000000ULL / s_rate;
        if (us) usleep((useconds_t)us);
    }
    return ESP_OK;
}

static esp_err_t sim_audio_stop(void)
{
    printf("[sim_audio] stop\n");
    s_played = 0;
    return ESP_OK;
}

static esp_err_t sim_audio_set_volume(uint8_t percent)
{
    printf("[sim_audio] set_volume %u%%\n", (unsigned)percent);
    return ESP_OK;
}

static esp_err_t sim_audio_configure(const hal_audio_config_t *cfg)
{
    printf("[sim_audio] configure sample_rate=%u bits=%u channels=%u\n",
           (unsigned)cfg->sample_rate,
           (unsigned)cfg->bits_per_sample,
           (unsigned)cfg->channels);
    if (cfg->sample_rate) s_rate = cfg->sample_rate;
    if (cfg->bits_per_sample) s_bits = cfg->bits_per_sample;
    if (cfg->channels) s_channels = cfg->channels;
    s_played = 0;
    return ESP_OK;
}

/* No host microphone is opened: a recording is silence, delivered at once. */
static esp_err_t sim_audio_record(uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    memset(buf, 0, len);
    printf("[sim_audio] record %zu bytes (silence)\n", len);
    return ESP_OK;
}

static esp_err_t sim_audio_set_mic_gain(uint8_t db)
{
    printf("[sim_audio] set_mic_gain %u dB\n", (unsigned)db);
    return ESP_OK;
}

/* The T-Deck Max's AUDIO_SEL: the speaker to the modem's voice, with the
 * amplifier on or off, or back to the codec. */
static esp_err_t sim_audio_set_route(hal_audio_route_t route, bool speaker)
{
    if (route == HAL_AUDIO_ROUTE_VOICE) {
        printf("[sim_audio] route voice, speaker %s\n", speaker ? "on" : "off");
        s_played = 0;
    } else if (route == HAL_AUDIO_ROUTE_CODEC) {
        printf("[sim_audio] route codec\n");
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    atomic_store(&s_route, (int)route);
    return ESP_OK;
}

static const hal_audio_driver_t sim_audio_driver = {
    .init      = sim_audio_init,
    .deinit    = sim_audio_deinit,
    .play      = sim_audio_play,
    .stop      = sim_audio_stop,
    .set_volume = sim_audio_set_volume,
    .configure = sim_audio_configure,
    .name      = "Simulator Audio",
    .record    = sim_audio_record,
    .set_mic_gain = sim_audio_set_mic_gain,
    .set_route = sim_audio_set_route,
};

const hal_audio_driver_t *sim_audio_get(void)
{
    return &sim_audio_driver;
}
