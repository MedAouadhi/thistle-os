/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 ThistleOS Contributors
 */
#include "sim_audio.h"
#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
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

/* The microphone. A recording is THISTLE_SIM_MIC, a WAV in the configured
 * format played in from its start; or, with a window, the host's own
 * microphone; or silence. It is paced as the ES8311 driver's is: a read
 * takes as long as its samples would, the first after a pause also the
 * 400 ms the driver drops while the microphone settles, and 700 ms with no
 * read closes it. */
#define MIC_SETTLE_US 400000
#define MIC_IDLE_US   700000

static pthread_mutex_t s_mic_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t  s_mic_last_us;    /* when a read last ran; 0 while closed */
static int64_t  s_mic_due_us;     /* when the samples handed out so far end */
static int      s_mic_busy;       /* reads under way */
static bool     s_mic_watching;   /* the thread that closes it is running */
static FILE    *s_mic_file;       /* at its samples */
static uint32_t s_mic_file_left;  /* bytes of them still to read */
static SDL_AudioDeviceID s_mic_dev;

static int64_t mic_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint32_t mic_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Opens `path` at its samples, if it is PCM in the format set. */
static bool mic_open_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("[sim_audio] cannot open THISTLE_SIM_MIC %s\n", path);
        return false;
    }
    uint8_t riff[12];
    unsigned tag = 0, channels = 0, bits = 0, rate = 0;
    uint32_t data = 0;
    bool found = fread(riff, 1, 12, f) == 12 && memcmp(riff, "RIFF", 4) == 0 &&
                 memcmp(riff + 8, "WAVE", 4) == 0;
    while (found) {
        uint8_t chunk[8];
        if (fread(chunk, 1, 8, f) != 8) {
            found = false;
            break;
        }
        uint32_t size = mic_le32(chunk + 4);
        if (memcmp(chunk, "data", 4) == 0) {
            data = size;
            break;
        }
        if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) != 16) {
                found = false;
                break;
            }
            tag = fmt[0] | (fmt[1] << 8);
            channels = fmt[2] | (fmt[3] << 8);
            rate = mic_le32(fmt + 4);
            bits = fmt[14] | (fmt[15] << 8);
            size -= 16;
        }
        fseek(f, (long)(size + (size & 1)), SEEK_CUR);
    }
    if (!found || tag != 1 || bits != 16 || rate != s_rate || channels != s_channels ||
        s_bits != 16) {
        printf("[sim_audio] THISTLE_SIM_MIC %s is not %u Hz %u-channel 16-bit PCM\n", path,
               (unsigned)s_rate, (unsigned)s_channels);
        fclose(f);
        return false;
    }
    s_mic_file = f;
    s_mic_file_left = data;
    return true;
}

static bool mic_open_host(void)
{
#ifdef __EMSCRIPTEN__
    return false;
#else
    extern bool sim_is_headless(void);
    if (sim_is_headless()) return false;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    SDL_AudioSpec want = {
        .freq = (int)s_rate,
        .format = AUDIO_S16LSB,
        .channels = s_channels,
        .samples = 1024,
    };
    SDL_AudioSpec have;
    s_mic_dev = SDL_OpenAudioDevice(NULL, 1, &want, &have, 0);
    if (!s_mic_dev) {
        printf("[sim_audio] no host microphone: %s\n", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(s_mic_dev, 0);
    return true;
#endif
}

static void mic_close_locked(void)
{
    if (s_mic_file) fclose(s_mic_file);
    s_mic_file = NULL;
    if (s_mic_dev) SDL_CloseAudioDevice(s_mic_dev);
    s_mic_dev = 0;
    s_mic_last_us = 0;
}

/* Closes the microphone once nothing has read it for MIC_IDLE_US, as the
 * driver's idle timer closes the codec: a host microphone left open shows
 * as in use. */
static void *mic_watch(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(100 * 1000);
        pthread_mutex_lock(&s_mic_lock);
        if (!s_mic_busy && s_mic_last_us && mic_now_us() - s_mic_last_us > MIC_IDLE_US) {
            mic_close_locked();
            printf("[sim_audio] microphone closed\n");
        }
        if (!s_mic_last_us && !s_mic_busy) {
            s_mic_watching = false;
            pthread_mutex_unlock(&s_mic_lock);
            return NULL;
        }
        pthread_mutex_unlock(&s_mic_lock);
    }
}

static esp_err_t sim_audio_record(uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;

    pthread_mutex_lock(&s_mic_lock);
    int64_t now = mic_now_us();
    bool opening = s_mic_last_us == 0 || now - s_mic_last_us > MIC_IDLE_US;
    if (opening) {
        mic_close_locked();
        const char *path = getenv("THISTLE_SIM_MIC");
        if (path && *path) mic_open_file(path);
        else mic_open_host();
        printf("[sim_audio] recording %u Hz %u-channel from %s\n", (unsigned)s_rate,
               (unsigned)s_channels,
               s_mic_file ? path : s_mic_dev ? "the host's microphone" : "silence");
        s_mic_due_us = now + MIC_SETTLE_US;
        if (!s_mic_watching) {
            pthread_t thread;
            if (pthread_create(&thread, NULL, mic_watch, NULL) == 0) {
                pthread_detach(thread);
                s_mic_watching = true;
            }
        }
    }
    s_mic_busy++;
    s_mic_last_us = now;

    size_t got = 0;
    if (s_mic_file) {
        size_t want = len < s_mic_file_left ? len : s_mic_file_left;
        got = fread(buf, 1, want, s_mic_file);
        s_mic_file_left -= (uint32_t)got;
    }
    memset(buf + got, 0, len - got);
    size_t frame = (size_t)s_channels * (s_bits / 8);
    int64_t settled = s_mic_due_us;   /* when settling ends, if opening */
    s_mic_due_us += frame && s_rate ? (int64_t)(len / frame) * 1000000 / s_rate : 0;
    int64_t due = s_mic_due_us;
    SDL_AudioDeviceID dev = s_mic_dev;
    pthread_mutex_unlock(&s_mic_lock);

    if (dev) {
        /* What the host heard while the codec would have been settling is
         * dropped, as the driver drops it; then SDL paces the reads. */
        if (opening) {
            int64_t wait = settled - mic_now_us();
            if (wait > 0) usleep((useconds_t)wait);
            SDL_ClearQueuedAudio(dev);
        }
        size_t have = 0;
        int64_t give_up = due + 1000000;
        while (have < len && mic_now_us() < give_up) {
            have += SDL_DequeueAudio(dev, buf + have, (Uint32)(len - have));
            if (have < len) usleep(5000);
        }
        if (have < len) memset(buf + have, 0, len - have);
    } else {
        int64_t wait = due - mic_now_us();
        if (wait > 0) usleep((useconds_t)wait);
    }

    pthread_mutex_lock(&s_mic_lock);
    s_mic_busy--;
    s_mic_last_us = mic_now_us();
    pthread_mutex_unlock(&s_mic_lock);
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
