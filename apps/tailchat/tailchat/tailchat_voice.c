// SPDX-License-Identifier: BSD-3-Clause
/* Voice messages' audio: see tailchat_voice.h. */
#include "tailchat_voice.h"

#include "tailchat_ogg.h"

#include "hal/board.h"
#include "thistle/audio_volume.h"
#include "thistle/call.h"
#include "thistle/power_manager.h"
#include "thistle_opus.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#ifdef SIMULATOR_BUILD
#include "esp_system.h"
#else
#include "esp_random.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "tailchat_voice";

/* Ticket 01: libopus used 14.4 KB of a task's C stack beside its PSRAM
 * pseudostack. */
#define TASK_STACK    (20 * 1024)
#define TASK_PRIORITY 5

#define FRAME_SAMPLES THISTLE_OPUS_FRAME               /* 20 ms at 16 kHz */
#define FRAME_BYTES   (FRAME_SAMPLES * 2)
#define FRAME_MS      20
#define FRAMES_MAX    (TAILCHAT_VOICE_MAX_MS / FRAME_MS)
/* The protocol's limit on a voice message's file. */
#define FILE_MAX      (512 * 1024)
/* Packets are kept under the file's limit, less its pages' headers. */
#define PACKETS_MAX   (FILE_MAX - 16 * 1024)
/* Frames decoded per play() call: 200 ms. */
#define PLAY_FRAMES   10

typedef struct {
    bool recording;
    char id[33];
    char path[160];
    atomic_bool stop;
    atomic_bool done;
    atomic_uint ms;
    const char *why;          /* a recording that is no message: why */
    uint8_t *file;            /* the recording's Ogg/Opus, or the file to play */
    size_t file_len;
    uint32_t file_ms;
    uint32_t hold;
} job_t;

static job_t *s_job;

static void *psram(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(size);
}

static const hal_audio_driver_t *audio(void)
{
    const hal_registry_t *reg = hal_get_registry();
    return reg ? reg->audio : NULL;
}

static esp_err_t speech_format(const hal_audio_driver_t *a)
{
    hal_audio_config_t format = {.sample_rate = THISTLE_OPUS_RATE, .bits_per_sample = 16, .channels = 1};
    return a->configure ? a->configure(&format) : ESP_OK;
}

static void finish(job_t *job)
{
#ifndef SIMULATOR_BUILD
    ESP_LOGI(TAG, "%s done; %u bytes of stack to spare", job->recording ? "recording" : "playing",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
    thistle_opus_give();
    power_manager_release(job->hold);
    atomic_store(&job->done, true);
    vTaskDelete(NULL);
}

/* Record 20 ms frames and encode each as it comes, up to 2 minutes: only
 * the Opus packets are kept, then made into the file. */
static void record_task(void *arg)
{
    job_t *job = arg;
    const hal_audio_driver_t *a = audio();
    uint8_t *packets = psram(PACKETS_MAX);
    uint16_t *lens = psram(FRAMES_MAX * sizeof(uint16_t));
    int16_t *pcm = psram(FRAME_BYTES);
    OpusEncoder *enc = thistle_opus_encoder_new(THISTLE_OPUS_BITRATE);
    size_t frames = 0, used = 0;
    opus_int32 lookahead = 0;

    rs_audio_capture_begin();
    esp_err_t err = packets && lens && pcm && enc ? speech_format(a) : ESP_ERR_NO_MEM;
    if (enc) opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));
    while (err == ESP_OK && !atomic_load(&job->stop) && frames < FRAMES_MAX &&
           used + THISTLE_OPUS_MAX_PACKET <= PACKETS_MAX) {
        /* A call that comes takes the audio and Opus: the recording is
         * dropped. */
        if (rs_call_in_progress()) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        err = a->record((uint8_t *)pcm, FRAME_BYTES);
        if (err != ESP_OK) break;
        int n = opus_encode(enc, pcm, FRAME_SAMPLES, packets + used, THISTLE_OPUS_MAX_PACKET);
        if (n < 0) {
            ESP_LOGW(TAG, "encode: %d", n);
            err = ESP_FAIL;
            break;
        }
        lens[frames++] = (uint16_t)n;
        used += (size_t)n;
        atomic_store(&job->ms, (unsigned)(frames * FRAME_MS));
    }
    rs_audio_capture_end();
    thistle_opus_encoder_free(enc);
    free(pcm);

    uint32_t ms = (uint32_t)(frames * FRAME_MS);
    if (err == ESP_ERR_INVALID_STATE) {
        job->why = "A call came; the recording was dropped";
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "recording failed: %s", esp_err_to_name(err));
        job->why = "The microphone failed";
    } else if (ms < TAILCHAT_VOICE_MIN_MS) {
        job->why = "Hold to record a voice message";
    } else {
        job->file = psram(FILE_MAX);
        job->file_len = job->file ? tailchat_ogg_write(job->file, FILE_MAX, packets, lens, frames,
                                                       (uint16_t)(lookahead * 3), esp_random())
                                  : 0;
        job->file_ms = ms;
        if (!job->file_len) job->why = "Not enough memory for the recording";
        ESP_LOGI(TAG, "recorded %u ms: %u frames, %u bytes", (unsigned)ms, (unsigned)frames,
                 (unsigned)job->file_len);
    }
    free(packets);
    free(lens);
    finish(job);
}

static bool read_file(job_t *job)
{
    FILE *f = fopen(job->path, "rb");
    if (!f) return false;
    job->file = psram(FILE_MAX);
    job->file_len = job->file ? fread(job->file, 1, FILE_MAX, f) : 0;
    fclose(f);
    return job->file_len > 0;
}

/* Decode the file's packets 200 ms at a time and play them. */
static void play_task(void *arg)
{
    job_t *job = arg;
    const hal_audio_driver_t *a = audio();
    uint8_t *packets = psram(FILE_MAX);
    uint16_t *lens = psram((FRAMES_MAX + 100) * sizeof(uint16_t));
    int16_t *pcm = psram(PLAY_FRAMES * FRAME_BYTES);
    OpusDecoder *dec = thistle_opus_decoder_new();
    tailchat_ogg_info_t info = {0};

    bool ok = packets && lens && pcm && dec && read_file(job) &&
              tailchat_ogg_read(job->file, job->file_len, packets, FILE_MAX, lens, FRAMES_MAX + 100, &info);
    free(job->file);
    job->file = NULL;
    if (!ok) ESP_LOGW(TAG, "cannot play %s: not a whole Ogg/Opus file", job->path);

    if (ok) {
        rs_audio_capture_begin();
        esp_err_t err = speech_format(a);
        size_t skip = info.pre_skip / 3;   /* 48 kHz samples to ours */
        size_t at = 0, filled = 0, played = 0;
        ESP_LOGI(TAG, "playing %s: %u ms", job->id, (unsigned)tailchat_ogg_ms(&info));
        /* A call that comes stops it. */
        for (size_t i = 0; err == ESP_OK && i < info.count && !atomic_load(&job->stop) && !rs_call_in_progress();
             i++) {
            int n = opus_decode(dec, packets + at, lens[i], pcm + filled, FRAME_SAMPLES, 0);
            at += lens[i];
            if (n <= 0) continue;
            size_t got = (size_t)n;
            if (skip) {
                size_t drop = skip < got ? skip : got;
                memmove(pcm + filled, pcm + filled + drop, (got - drop) * 2);
                got -= drop;
                skip -= drop;
            }
            filled += got;
            if (filled >= (PLAY_FRAMES - 1) * FRAME_SAMPLES || i + 1 == info.count) {
                err = a->play((const uint8_t *)pcm, filled * 2);
                played += filled;
                filled = 0;
                atomic_store(&job->ms, (unsigned)(played / (THISTLE_OPUS_RATE / 1000)));
            }
        }
        if (err != ESP_OK) ESP_LOGW(TAG, "playing failed: %s", esp_err_to_name(err));
        rs_audio_capture_end();
    }
    thistle_opus_decoder_free(dec);
    free(packets);
    free(lens);
    free(pcm);
    finish(job);
}

static void job_free(void)
{
    if (!s_job) return;
    free(s_job->file);
    free(s_job);
    s_job = NULL;
}

/* A job may start: none running, nothing else on the audio or Opus. */
static const char *may_start(void)
{
    if (s_job && !atomic_load(&s_job->done)) return "Already busy with a voice message";
    if (s_job && s_job->recording && (s_job->file || s_job->why)) return "A recording is still being sent";
    const hal_audio_driver_t *a = audio();
    if (!a || !a->configure) return "This device has no audio";
    if (rs_call_in_progress()) return "Not during a call";
    return NULL;
}

static const char *start(bool recording, const char *path, const char *id)
{
    const char *why = may_start();
    if (why) return why;
    if (recording && !audio()->record) return "This device has no microphone";
    if (!recording && !audio()->play) return "This device has no speaker";
    job_free();
    const char *holder = NULL;
    if (!thistle_opus_take(recording ? "a voice recording" : "a voice message", &holder)) {
        static char busy[64];
        snprintf(busy, sizeof busy, "Busy with %s", holder ? holder : "audio");
        return busy;
    }
    job_t *job = calloc(1, sizeof *job);
    if (!job) {
        thistle_opus_give();
        return "Not enough memory";
    }
    job->recording = recording;
    if (path) snprintf(job->path, sizeof job->path, "%s", path);
    if (id) snprintf(job->id, sizeof job->id, "%s", id);
    job->hold = power_manager_hold(recording ? "recording a voice message" : "playing a voice message");
    if (xTaskCreate(recording ? record_task : play_task, recording ? "voice_rec" : "voice_play", TASK_STACK,
                    job, TASK_PRIORITY, NULL) != pdPASS) {
        power_manager_release(job->hold);
        thistle_opus_give();
        free(job);
        return "Could not start the audio";
    }
    s_job = job;
    ESP_LOGI(TAG, "%s%s", recording ? "recording" : "playing ", recording ? "" : job->id);
    return NULL;
}

const char *tailchat_voice_record(void)
{
    return start(true, NULL, NULL);
}

const char *tailchat_voice_play(const char *path, const char *id)
{
    return start(false, path, id);
}

void tailchat_voice_stop(void)
{
    if (s_job) atomic_store(&s_job->stop, true);
}

tailchat_voice_state_t tailchat_voice_poll(uint32_t *ms, const char **id)
{
    if (ms) *ms = s_job ? atomic_load(&s_job->ms) : 0;
    if (id) *id = s_job && !s_job->recording ? s_job->id : "";
    if (!s_job) return TAILCHAT_VOICE_IDLE;
    if (!atomic_load(&s_job->done)) return s_job->recording ? TAILCHAT_VOICE_RECORDING : TAILCHAT_VOICE_PLAYING;
    if (s_job->recording) return TAILCHAT_VOICE_RECORDED;
    job_free();
    if (id) *id = "";
    return TAILCHAT_VOICE_IDLE;
}

const uint8_t *tailchat_voice_take(size_t *len, uint32_t *ms, const char **why)
{
    *len = 0;
    *ms = 0;
    *why = NULL;
    if (!s_job || !s_job->recording || !atomic_load(&s_job->done)) return NULL;
    if (!s_job->file || !s_job->file_len) {
        *why = s_job->why ? s_job->why : "Nothing was recorded";
        return NULL;
    }
    *len = s_job->file_len;
    *ms = s_job->file_ms;
    return s_job->file;
}

void tailchat_voice_release(void)
{
    if (s_job && atomic_load(&s_job->done)) job_free();
}
