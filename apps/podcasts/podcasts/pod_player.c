/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — plays an episode, streamed or from the SD card.
 *
 * Two threads and a ring:
 *
 *   fetch   the MP3 from the network or the card into the ring, 512 KB in
 *           PSRAM, half a minute of a typical episode; reconnects where it
 *           was if the connection drops; writes where the episode was left
 *           to the library every 30 s, since it is the thread with time to
 *           spare.
 *   decode  frames out of the ring with minimp3, and the PCM to the audio
 *           HAL, whose play() blocks until the DMA has room: that is what
 *           paces it.
 *
 * Both work for a session, a number that a new episode, a stop or a 10 minute
 * pause moves on; a thread whose session has passed finishes the call it is
 * in and goes, touching nothing. Within a session the epoch moves on at each
 * seek: the ring starts over from a new byte, and the decoder with it. All of
 * it under one mutex, never held across I/O.
 *
 * Only one decoder runs at a time, whatever the sessions: minimp3's scratch
 * is a single static (pod_mp3.c), and the codec has one format.
 */
#include "podcasts/pod_player.h"
#include "podcasts/pod_download.h"
#include "podcasts/pod_library.h"
#include "podcasts/pod_net.h"
#include "podcasts/pod_text.h"

#include "third_party/minimp3/minimp3.h"

#include "hal/board.h"
#include "thistle/audio_volume.h"
#include "thistle/call.h"
#include "thistle/power_manager.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "pod_player";

#define RING_SIZE        (512 * 1024)
#define CHUNK            (16 * 1024)    /* one read from the source */
#define WINDOW           (16 * 1024)    /* what minimp3 is given to find a frame in */
#define MIN_DECODE       (4 * 1024)     /* less than this, and not the end: wait */
#define FETCH_STACK      8192
#define DECODE_STACK     8192
#define FETCH_PRIORITY   5
#define DECODE_PRIORITY  7              /* above the LVGL task's 5 */
#define SAVE_EVERY_US    (30LL * 1000 * 1000)
#define PARK_AFTER_US    (10LL * 60 * 1000 * 1000)
#define RETRIES          4
#define SILENCE_MS       20
/* Played to within this of the end counts as finished. */
#define FINISHED_SLACK_S 30

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
/* Held by a decode thread from start to end: one decoder at a time. */
static pthread_mutex_t s_decoder = PTHREAD_MUTEX_INITIALIZER;

/* In PSRAM, as buffers only tasks touch belong (CLAUDE.md): the episode in
 * it alone is 2 KB. */
static EXT_RAM_BSS_ATTR struct {
    /* What is loaded */
    pod_episode_t      episode;
    bool               loaded;
    pod_player_state_t state;
    char               error[80];
    char               path[192];     /* the saved file, or "" to stream */

    uint32_t session;                 /* moves on: new episode, stop, park */
    uint32_t epoch;                   /* moves on: new session or seek */
    bool     running;                 /* this session has threads */

    /* The source */
    int64_t  fetch_from;              /* the byte the ring starts at this epoch */
    int64_t  total;                   /* the file's length; 0 when not known */
    int64_t  audio_start;             /* past its ID3 tag; -1 until known */

    /* The ring */
    uint8_t *ring;
    size_t   head, tail, fill;
    bool     eof;                     /* the source has no more */
    bool     source_failed;
    char     source_error[80];

    /* Time */
    int32_t  base_s;                  /* where this epoch began, in seconds */
    uint64_t samples;                 /* played since then, per channel */
    uint32_t hz;
    int      kbps;
    bool     paused;
    int64_t  paused_at_us;
    bool     paused_for_capture;      /* by a recording, to play on after it */
    bool     buffering;
    uint32_t hold;                    /* power_manager_hold() while playing */
} s;

static void *psram_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(size);
}

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

/* ------------------------------------------------------------------ */
/* State, under s_lock                                                  */
/* ------------------------------------------------------------------ */

static int32_t position_locked(void)
{
    int32_t pos = s.base_s + (s.hz ? (int32_t)(s.samples / s.hz) : 0);
    return pos < 0 ? 0 : pos;
}

static int32_t duration_locked(void)
{
    if (s.episode.duration > 0) return s.episode.duration;
    /* None given: the length at the bitrate heard. */
    if (s.total > 0 && s.kbps > 0) {
        int64_t audio = s.total - (s.audio_start > 0 ? s.audio_start : 0);
        return (int32_t)(audio / (s.kbps * 125));
    }
    return 0;
}

/* The byte `seconds` into the audio: by its length over its duration, which
 * is right for a constant bitrate and near enough for a variable one. */
static int64_t offset_of_locked(int32_t seconds)
{
    if (seconds <= 0) return 0;
    int64_t start = s.audio_start > 0 ? s.audio_start : 0;
    int64_t total = s.total > 0 ? s.total : s.episode.bytes;
    int32_t duration = duration_locked();
    if (total > start && duration > 0) {
        int64_t at = start + (total - start) * seconds / duration;
        return at < total ? at : total;
    }
    if (s.kbps > 0) return start + (int64_t)seconds * s.kbps * 125;
    return 0;
}

static void hold_locked(bool on)
{
    if (on && !s.hold) s.hold = power_manager_hold("a podcast is playing");
    if (!on && s.hold) {
        power_manager_release(s.hold);
        s.hold = 0;
    }
}

/* Starts the ring over from `seconds` in. */
static void restart_at_locked(int32_t seconds)
{
    if (seconds < 0) seconds = 0;
    int32_t duration = duration_locked();
    if (duration > 0 && seconds > duration) seconds = duration;
    s.epoch++;
    s.fetch_from = offset_of_locked(seconds);
    /* Not knowing the bytes, start from the top, and say so. */
    s.base_s = s.fetch_from || !seconds ? seconds : 0;
    s.samples = 0;
    s.head = s.tail = s.fill = 0;
    s.eof = s.source_failed = false;
    s.source_error[0] = '\0';
    s.buffering = true;
}

static void ring_push_locked(const uint8_t *data, size_t n)
{
    while (n) {
        size_t run = RING_SIZE - s.head;
        if (run > n) run = n;
        memcpy(s.ring + s.head, data, run);
        s.head = (s.head + run) % RING_SIZE;
        s.fill += run;
        data += run;
        n -= run;
    }
}

static size_t ring_pop_locked(uint8_t *out, size_t max)
{
    size_t n = max < s.fill ? max : s.fill, done = 0;
    while (done < n) {
        size_t run = RING_SIZE - s.tail;
        if (run > n - done) run = n - done;
        memcpy(out + done, s.ring + s.tail, run);
        s.tail = (s.tail + run) % RING_SIZE;
        done += run;
    }
    s.fill -= n;
    return n;
}

/* ------------------------------------------------------------------ */
/* The library                                                          */
/* ------------------------------------------------------------------ */

/* Where the loaded episode was left, taken under the lock to be written
 * once it is let go: each a copy of its own, in PSRAM, since any thread may
 * be saving at once. */
typedef struct {
    pod_episode_t episode;
    int32_t       position;
    int32_t       duration;
    bool          ended;
} snapshot_t;

/* NULL when nothing is loaded, or `session` (0 for any) has passed. */
static snapshot_t *snapshot_locked(uint32_t session)
{
    if (!s.loaded || (session && s.session != session)) return NULL;
    snapshot_t *snap = psram_alloc(sizeof *snap);
    if (!snap) return NULL;
    snap->episode = s.episode;
    snap->position = position_locked();
    snap->duration = duration_locked();
    snap->ended = s.state == POD_PLAYER_ENDED;
    return snap;
}

/* Writes it to the library and frees it. With s_lock not held: it is SD
 * I/O. */
static void save_snapshot(snapshot_t *snap)
{
    if (!snap) return;
    bool finished = snap->ended ||
                    (snap->duration > 0 && snap->position >= snap->duration - FINISHED_SLACK_S);
    pod_lib_play_episode(&snap->episode, snap->position, finished, (int64_t)time(NULL));
    pod_lib_flush();
    free(snap);
}

/* ------------------------------------------------------------------ */
/* The fetch thread                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    pod_stream_t *stream;
    FILE         *file;
} source_t;

static void source_close(source_t *src)
{
    if (src->stream) pod_net_close(src->stream);
    if (src->file) fclose(src->file);
    src->stream = NULL;
    src->file = NULL;
}

/* Opens the episode at byte `from`: the byte it really starts at in
 * `*start`, the file's length in `*total`. */
static bool source_open(source_t *src, const char *path, const char *url, int64_t from,
                        int64_t *start, int64_t *total, char *error, size_t cap)
{
    if (path[0]) {
        src->file = fopen(path, "rb");
        if (!src->file) {
            pod_text_copy(error, cap, "The saved file could not be opened");
            return false;
        }
        fseek(src->file, 0, SEEK_END);
        *total = ftell(src->file);
        if (from > *total) from = *total;
        fseek(src->file, (long)from, SEEK_SET);
        *start = from;
        return true;
    }
    src->stream = pod_net_open(url, from, total, start, error, cap);
    return src->stream != NULL;
}

static int source_read(source_t *src, uint8_t *buf, int len)
{
    if (src->file) {
        size_t n = fread(buf, 1, (size_t)len, src->file);
        return n > 0 ? (int)n : ferror(src->file) ? -1 : 0;
    }
    return pod_net_read(src->stream, buf, len);
}

static void fetch_task(void *arg)
{
    const uint32_t me = (uint32_t)(uintptr_t)arg;
    uint8_t *chunk = psram_alloc(CHUNK);
    /* The URL, up to 768 bytes, and the path, are copied once: they do not
     * change within a session. */
    char *url = psram_alloc(POD_URL_MAX);
    char path[sizeof s.path];
    source_t src = {0};
    uint32_t epoch = 0;
    int64_t at = 0;          /* the next byte to fetch */
    int64_t skip = 0;        /* bytes before `at` the source sent anyway */
    bool open = false;
    int failures = 0;
    int64_t saved_us = now_us();

    pthread_mutex_lock(&s_lock);
    bool ok = chunk && url && s.session == me;
    if (ok) {
        pod_text_copy(url, POD_URL_MAX, s.episode.url);
        pod_text_copy(path, sizeof path, s.path);
    }
    pthread_mutex_unlock(&s_lock);

    while (ok) {
        pthread_mutex_lock(&s_lock);
        if (s.session != me) {
            pthread_mutex_unlock(&s_lock);
            break;
        }
        bool restart = s.epoch != epoch;
        if (restart) {
            epoch = s.epoch;
            at = s.fetch_from;
            failures = 0;
        }
        size_t room = RING_SIZE - s.fill;
        bool idle = s.eof || s.source_failed;
        bool playing = s.state == POD_PLAYER_PLAYING && !s.paused;
        pthread_mutex_unlock(&s_lock);

        if (restart && open) {
            source_close(&src);
            open = false;
        }

        /* Where the episode was left, now and then: here, since this thread
         * has time to spare and the decoder has none. */
        if (playing && now_us() - saved_us >= SAVE_EVERY_US) {
            pthread_mutex_lock(&s_lock);
            snapshot_t *snap = snapshot_locked(me);
            pthread_mutex_unlock(&s_lock);
            save_snapshot(snap);
            saved_us = now_us();
        }

        if (idle || (open && room < CHUNK)) {
            vTaskDelay(pdMS_TO_TICKS(open ? 20 : 50));
            continue;
        }

        if (!open) {
            char error[80] = "";
            int64_t start = 0, total = 0;
            if (source_open(&src, path, url, at, &start, &total, error, sizeof error)) {
                open = true;
                skip = at - start;
                pthread_mutex_lock(&s_lock);
                if (s.session == me && s.epoch == epoch && total > 0) s.total = total;
                pthread_mutex_unlock(&s_lock);
                continue;
            }
            ESP_LOGW(TAG, "open at %lld: %s", (long long)at, error);
            if (path[0] || ++failures > RETRIES) {
                pthread_mutex_lock(&s_lock);
                if (s.session == me && s.epoch == epoch) {
                    s.source_failed = true;
                    pod_text_copy(s.source_error, sizeof s.source_error, error);
                }
                pthread_mutex_unlock(&s_lock);
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000 * failures));
            }
            continue;
        }

        size_t want = CHUNK < room ? CHUNK : room;
        if (skip > 0 && (int64_t)want > skip) want = (size_t)skip;
        int n = source_read(&src, chunk, (int)want);
        if (n > 0) {
            failures = 0;
            if (skip > 0) {
                skip -= n;
                continue;
            }
            pthread_mutex_lock(&s_lock);
            if (s.session == me && s.epoch == epoch) ring_push_locked(chunk, (size_t)n);
            pthread_mutex_unlock(&s_lock);
            at += n;
            continue;
        }

        /* The end, or the connection gone: which, the length says when the
         * server gave one. */
        pthread_mutex_lock(&s_lock);
        int64_t total = s.total;
        pthread_mutex_unlock(&s_lock);
        if (n == 0 && (total == 0 || at >= total)) {
            pthread_mutex_lock(&s_lock);
            if (s.session == me && s.epoch == epoch) s.eof = true;
            pthread_mutex_unlock(&s_lock);
            continue;
        }
        ESP_LOGW(TAG, "read at %lld of %lld: %d; reconnecting", (long long)at, (long long)total, n);
        source_close(&src);
        open = false;
    }

    source_close(&src);
    free(chunk);
    free(url);
#ifndef SIMULATOR_BUILD
    ESP_LOGI(TAG, "fetch done; %u bytes of stack to spare",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* The decode thread                                                    */
/* ------------------------------------------------------------------ */

/* The length of the ID3v2 tag `p` starts with, header and footer included,
 * or 0 when it starts with none. */
static int64_t id3_length(const uint8_t *p, size_t n)
{
    if (n < 10 || memcmp(p, "ID3", 3) != 0) return 0;
    if ((p[6] | p[7] | p[8] | p[9]) & 0x80) return 0;
    int64_t size = (int64_t)p[6] << 21 | p[7] << 14 | p[8] << 7 | p[9];
    return 10 + size + (p[5] & 0x10 ? 10 : 0);
}

/* Ends the session from the decode thread: played to the end, or failed. */
static void finish(uint32_t me, pod_player_state_t state, const char *error)
{
    snapshot_t *snap = NULL;
    pthread_mutex_lock(&s_lock);
    if (s.session == me) {
        s.state = state;
        if (error) pod_text_copy(s.error, sizeof s.error, error);
        snap = snapshot_locked(me);
        s.session++;          /* the fetch thread goes too */
        s.running = false;
        s.buffering = false;
        hold_locked(false);
    }
    pthread_mutex_unlock(&s_lock);
    save_snapshot(snap);
    if (state == POD_PLAYER_ENDED) ESP_LOGI(TAG, "played to the end");
    else ESP_LOGW(TAG, "stopped: %s", error ? error : "");
}

static void decode_task(void *arg)
{
    const uint32_t me = (uint32_t)(uintptr_t)arg;
    pthread_mutex_lock(&s_decoder);

    uint8_t *window = psram_alloc(WINDOW);
    int16_t *pcm = psram_alloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    const size_t silence_size = 48000 / 1000 * SILENCE_MS * 2 * sizeof(int16_t);
    int16_t *silence = psram_alloc(silence_size);
    if (silence) memset(silence, 0, silence_size);
    mp3dec_t *dec = psram_alloc(sizeof *dec);
    const hal_registry_t *reg = hal_get_registry();
    const hal_audio_driver_t *audio = reg ? reg->audio : NULL;

    size_t have = 0;
    uint32_t epoch = 0;
    bool checked_tag = false;
    int64_t skip = 0;
    uint32_t format_hz = 0;
    int format_channels = 0;
    bool was_paused = false;
    bool ok = window && pcm && silence && dec && audio && audio->play;
    if (!ok) finish(me, POD_PLAYER_FAILED, audio ? "Out of memory" : "This device has no audio output");

    while (ok) {
        pthread_mutex_lock(&s_lock);
        if (s.session != me) {
            pthread_mutex_unlock(&s_lock);
            break;
        }
        if (s.epoch != epoch) {
            epoch = s.epoch;
            have = 0;
            mp3dec_init(dec);
            /* Only the start of the file has a tag to step over. */
            checked_tag = s.fetch_from != 0;
            skip = 0;
        }
        bool paused = s.paused;
        int64_t paused_at = s.paused_at_us;
        bool play_on = paused && s.paused_for_capture && !rs_audio_capturing();
        if (!paused && have < WINDOW) have += ring_pop_locked(window + have, WINDOW - have);
        bool drained = s.fill == 0;
        bool at_end = s.eof && drained;
        bool failed = s.source_failed && drained;
        char why[80];
        pod_text_copy(why, sizeof why, s.source_error);
        pthread_mutex_unlock(&s_lock);

        /* A call has the audio: pause as the user would, keeping the place,
         * and stay paused after it; the speaker is the modem's meanwhile. */
        if (!paused && rs_call_in_progress()) {
            ESP_LOGI(TAG, "a call is up; pausing");
            pod_player_pause();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Dictation has the microphone, which hears the speaker, and the
         * codec, at its own rate: pause, and play on when it is done. */
        if (!paused && rs_audio_capturing()) {
            ESP_LOGI(TAG, "recording; pausing");
            pod_player_pause();
            pthread_mutex_lock(&s_lock);
            if (s.session == me && s.paused) s.paused_for_capture = true;
            pthread_mutex_unlock(&s_lock);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (play_on) {
            ESP_LOGI(TAG, "recording done; playing on");
            pod_player_resume();
            continue;
        }

        /* Whatever played meanwhile (a call's ringtone sets 16 kHz mono) may
         * have changed the codec's format: set ours again after a pause. */
        if (paused) {
            was_paused = true;
        } else if (was_paused) {
            was_paused = false;
            format_hz = 0;
            format_channels = 0;
        }

        if (paused) {
            if (now_us() - paused_at >= PARK_AFTER_US) {
                /* Long enough: the threads and the connection go, and
                 * resume() starts them again where this was. */
                pthread_mutex_lock(&s_lock);
                if (s.session == me && s.paused) {
                    s.session++;
                    s.running = false;
                    ESP_LOGI(TAG, "paused a while; letting go of the connection");
                }
                pthread_mutex_unlock(&s_lock);
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!checked_tag) {
            if (have < 10 && !at_end && !failed) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            skip = id3_length(window, have);
            checked_tag = true;
            pthread_mutex_lock(&s_lock);
            if (s.session == me && s.epoch == epoch) s.audio_start = skip;
            pthread_mutex_unlock(&s_lock);
            if (skip) ESP_LOGI(TAG, "skipping a %lld byte ID3 tag", (long long)skip);
        }
        if (skip > 0) {
            size_t drop = (int64_t)have < skip ? have : (size_t)skip;
            memmove(window, window + drop, have - drop);
            have -= drop;
            skip -= (int64_t)drop;
            if (skip > 0 && !have && !at_end && !failed) vTaskDelay(pdMS_TO_TICKS(5));
            if (skip > 0 && !at_end && !failed) continue;
        }

        if (have < MIN_DECODE && !at_end && !failed) {
            /* Waiting on the network. The codec closes, with a pop, after
             * 500 ms without data; silence keeps it open, so a stall is a
             * gap. Before the first sound there is no format to keep. */
            pthread_mutex_lock(&s_lock);
            if (s.session == me) s.buffering = true;
            pthread_mutex_unlock(&s_lock);
            if (format_hz) {
                size_t frames = format_hz / 1000 * SILENCE_MS;
                audio->play((const uint8_t *)silence, frames * (size_t)format_channels * sizeof(int16_t));
            } else {
                vTaskDelay(pdMS_TO_TICKS(SILENCE_MS));
            }
            continue;
        }
        if (have == 0) {
            if (failed) finish(me, POD_PLAYER_FAILED, why[0] ? why : "The connection was lost");
            else finish(me, POD_PLAYER_ENDED, NULL);
            break;
        }

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, window, (int)have, pcm, &info);
        if (info.frame_bytes == 0) {
            /* No frame in what there is. At the end, that is the end; with a
             * full window, it is not audio, and goes. */
            if (at_end || failed || have == WINDOW) have = 0;
            continue;
        }
        size_t used = (size_t)info.frame_bytes < have ? (size_t)info.frame_bytes : have;
        memmove(window, window + used, have - used);
        have -= used;
        if (samples <= 0) continue;

        if ((uint32_t)info.hz != format_hz || info.channels != format_channels) {
            hal_audio_config_t format = {
                .sample_rate = (uint32_t)info.hz,
                .bits_per_sample = 16,
                .channels = (uint8_t)info.channels,
            };
            if (audio->configure) audio->configure(&format);
            format_hz = (uint32_t)info.hz;
            format_channels = info.channels;
            ESP_LOGI(TAG, "%d Hz, %d channel%s, %d kbps", info.hz, info.channels,
                     info.channels == 1 ? "" : "s", info.bitrate_kbps);
        }

        /* Nothing goes to the codec for a session or a seek that has passed. */
        pthread_mutex_lock(&s_lock);
        bool current = s.session == me && s.epoch == epoch;
        pthread_mutex_unlock(&s_lock);
        if (!current) continue;

        audio->play((const uint8_t *)pcm, (size_t)samples * (size_t)info.channels * sizeof(int16_t));

        pthread_mutex_lock(&s_lock);
        if (s.session == me && s.epoch == epoch) {
            if (s.hz && s.hz != (uint32_t)info.hz) {
                /* A rate change mid-file: keep the time heard so far. */
                s.base_s = position_locked();
                s.samples = 0;
            }
            s.samples += (uint64_t)samples;
            s.hz = (uint32_t)info.hz;
            if (info.bitrate_kbps > 0) s.kbps = info.bitrate_kbps;
            s.buffering = false;
            if (s.state == POD_PLAYER_LOADING) s.state = POD_PLAYER_PLAYING;
        }
        pthread_mutex_unlock(&s_lock);
    }

    free(window);
    free(pcm);
    free(silence);
    free(dec);
#ifndef SIMULATOR_BUILD
    ESP_LOGI(TAG, "decode done; %u bytes of stack to spare",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
    pthread_mutex_unlock(&s_decoder);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Control                                                              */
/* ------------------------------------------------------------------ */

/* Starts a session's threads, s_lock held. */
static bool start_locked(void)
{
    if (!s.ring) s.ring = psram_alloc(RING_SIZE);
    if (!s.ring) {
        pod_text_copy(s.error, sizeof s.error, "Out of memory");
        s.state = POD_PLAYER_FAILED;
        return false;
    }
    s.session++;
    s.running = true;
    void *arg = (void *)(uintptr_t)s.session;
    if (xTaskCreate(fetch_task, "pod_fetch", FETCH_STACK, arg, FETCH_PRIORITY, NULL) != pdPASS) {
        s.session++;
        s.running = false;
        pod_text_copy(s.error, sizeof s.error, "Not enough memory to play");
        s.state = POD_PLAYER_FAILED;
        return false;
    }
    if (xTaskCreate(decode_task, "pod_decode", DECODE_STACK, arg, DECODE_PRIORITY, NULL) != pdPASS) {
        s.session++;          /* the fetch thread sees it and goes */
        s.running = false;
        pod_text_copy(s.error, sizeof s.error, "Not enough memory to play");
        s.state = POD_PLAYER_FAILED;
        return false;
    }
    return true;
}

const char *pod_player_play(const pod_episode_t *episode, int32_t from)
{
    if (!pod_episode_playable(episode)) {
        return episode->url[0] ? "Only MP3 episodes can be played" : "This episode has no audio";
    }
    const hal_registry_t *reg = hal_get_registry();
    if (!reg || !reg->audio || !reg->audio->play) return "This device has no audio output";

    char path[sizeof s.path];
    pod_download_path(episode, path, sizeof path);
    struct stat st;
    bool local = stat(path, &st) == 0 && st.st_size > 0;
    if (!local && !pod_net_connected()) {
        return "No network, and this episode is not saved to the SD card";
    }

    pod_lib_episode_t known;
    if (from < 0) {
        from = pod_lib_episode(episode->id, &known) && !known.finished ? known.position : 0;
    }
    /* Begun within a few seconds of the end: from the top. */
    if (episode->duration > 0 && from >= episode->duration - 5) from = 0;

    /* What was playing keeps its place. */
    pthread_mutex_lock(&s_lock);
    snapshot_t *old = s.loaded && s.episode.id != episode->id && s.state != POD_PLAYER_ENDED
                          ? snapshot_locked(0) : NULL;
    pthread_mutex_unlock(&s_lock);
    save_snapshot(old);

    pthread_mutex_lock(&s_lock);
    s.session++;              /* whatever was running goes */
    s.episode = *episode;
    s.loaded = true;
    s.state = POD_PLAYER_LOADING;
    s.error[0] = '\0';
    pod_text_copy(s.path, sizeof s.path, local ? path : "");
    s.total = local ? (int64_t)st.st_size : episode->bytes;
    s.audio_start = -1;
    s.hz = 0;
    s.kbps = 0;
    s.paused = false;
    s.paused_for_capture = false;
    restart_at_locked(from);
    hold_locked(true);
    bool started = start_locked();
    if (!started) hold_locked(false);
    pthread_mutex_unlock(&s_lock);
    if (!started) return "Not enough memory to play";

    ESP_LOGI(TAG, "playing %lld from %d s, %s", (long long)episode->id, (int)from,
             local ? "from the SD card" : "streamed");
    pod_lib_play_episode(episode, from, false, (int64_t)time(NULL));
    pod_lib_flush();
    return NULL;
}

void pod_player_pause(void)
{
    snapshot_t *snap = NULL;
    pthread_mutex_lock(&s_lock);
    /* Paused by the user: stays paused after a recording. */
    s.paused_for_capture = false;
    if (s.loaded && (s.state == POD_PLAYER_PLAYING || s.state == POD_PLAYER_LOADING)) {
        s.paused = true;
        s.paused_at_us = now_us();
        s.state = POD_PLAYER_PAUSED;
        s.buffering = false;
        hold_locked(false);
        snap = snapshot_locked(0);
    }
    pthread_mutex_unlock(&s_lock);
    /* The codec goes quiet by itself, half a second after the last sound. */
    save_snapshot(snap);
}

void pod_player_resume(void)
{
    pthread_mutex_lock(&s_lock);
    s.paused_for_capture = false;
    if (!s.loaded) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    if (s.state == POD_PLAYER_ENDED || s.state == POD_PLAYER_FAILED) {
        /* Again: from the top if it ended, from where it stopped if not. */
        int32_t from = s.state == POD_PLAYER_ENDED ? 0 : position_locked();
        pod_episode_t *again = psram_alloc(sizeof *again);
        if (again) *again = s.episode;
        pthread_mutex_unlock(&s_lock);
        const char *why = again ? pod_player_play(again, from) : "Out of memory";
        free(again);
        if (why) {
            pthread_mutex_lock(&s_lock);
            s.state = POD_PLAYER_FAILED;
            pod_text_copy(s.error, sizeof s.error, why);
            pthread_mutex_unlock(&s_lock);
        }
        return;
    }
    if (s.state == POD_PLAYER_PAUSED) {
        s.paused = false;
        hold_locked(true);
        if (s.running) {
            s.state = s.hz ? POD_PLAYER_PLAYING : POD_PLAYER_LOADING;
        } else {
            /* Let go of while paused: start again where it was. */
            s.state = POD_PLAYER_LOADING;
            restart_at_locked(position_locked());
            if (!start_locked()) hold_locked(false);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

void pod_player_toggle(void)
{
    pthread_mutex_lock(&s_lock);
    bool playing = s.loaded && (s.state == POD_PLAYER_PLAYING || s.state == POD_PLAYER_LOADING);
    pthread_mutex_unlock(&s_lock);
    if (playing) pod_player_pause();
    else pod_player_resume();
}

void pod_player_seek(int32_t position)
{
    pthread_mutex_lock(&s_lock);
    if (s.loaded && s.state != POD_PLAYER_IDLE) {
        if (s.state == POD_PLAYER_ENDED || s.state == POD_PLAYER_FAILED) {
            /* Seeking a finished one plays it from there. */
            s.state = POD_PLAYER_LOADING;
            s.paused = false;
            restart_at_locked(position);
            hold_locked(true);
            if (!start_locked()) hold_locked(false);
        } else if (!s.running) {
            /* Parked: the new place is kept for resume(). */
            s.base_s = position < 0 ? 0 : position;
            s.samples = 0;
        } else {
            restart_at_locked(position);
            if (!s.paused) s.state = POD_PLAYER_LOADING;
        }
    }
    pthread_mutex_unlock(&s_lock);
}

void pod_player_skip(int32_t seconds)
{
    pthread_mutex_lock(&s_lock);
    int32_t position = s.loaded ? position_locked() : 0;
    bool loaded = s.loaded;
    pthread_mutex_unlock(&s_lock);
    if (loaded) pod_player_seek(position + seconds);
}

void pod_player_stop(void)
{
    pthread_mutex_lock(&s_lock);
    snapshot_t *snap = s.state != POD_PLAYER_ENDED ? snapshot_locked(0) : NULL;
    s.session++;
    s.running = false;
    s.loaded = false;
    s.state = POD_PLAYER_IDLE;
    s.paused = false;
    s.paused_for_capture = false;
    s.buffering = false;
    hold_locked(false);
    /* The threads of the session that passed touch the ring only under the
     * lock, and only while it is theirs, so it can go now. */
    free(s.ring);
    s.ring = NULL;
    s.head = s.tail = s.fill = 0;
    pthread_mutex_unlock(&s_lock);
    save_snapshot(snap);
    const hal_registry_t *reg = hal_get_registry();
    if (reg && reg->audio && reg->audio->stop) reg->audio->stop();
}

void pod_player_status(pod_player_status_t *out)
{
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&s_lock);
    if (s.loaded) {
        out->state = s.state;
        out->episode_id = s.episode.id;
        out->position = position_locked();
        out->duration = duration_locked();
        if (out->duration && out->position > out->duration) out->position = out->duration;
        out->buffering = s.buffering && s.state == POD_PLAYER_PLAYING;
        out->local = s.path[0] != '\0';
        pod_text_copy(out->error, sizeof out->error, s.error);
    }
    pthread_mutex_unlock(&s_lock);
}

bool pod_player_episode(pod_episode_t *out)
{
    pthread_mutex_lock(&s_lock);
    bool loaded = s.loaded;
    if (loaded) *out = s.episode;
    pthread_mutex_unlock(&s_lock);
    return loaded;
}

bool pod_player_active(void)
{
    pthread_mutex_lock(&s_lock);
    bool active = s.loaded && (s.state == POD_PLAYER_PLAYING || s.state == POD_PLAYER_LOADING ||
                               s.state == POD_PLAYER_PAUSED);
    pthread_mutex_unlock(&s_lock);
    return active;
}
