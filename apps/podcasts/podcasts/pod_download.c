/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — episodes saved to the SD card.
 */
#include "podcasts/pod_download.h"
#include "podcasts/pod_library.h"
#include "podcasts/pod_net.h"
#include "podcasts/pod_text.h"

#include "hal/board.h"
#include "hal/sdcard_path.h"
#include "thistle/power_manager.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "pod_download";

#define SAVE_DIR       THISTLE_SDCARD "/podcasts"
#define CHUNK          (32 * 1024)
#define TASK_STACK     8192
#define TASK_PRIORITY  4
#define RETRIES        4
/* Room left on the card after an episode, for everything else on it. */
#define CARD_MARGIN    (16LL * 1024 * 1024)

typedef struct {
    pod_episode_t  episode;
    pod_dl_state_t state;
    int64_t        done;
    int64_t        total;
    char           error[64];
    bool           cancel;
} job_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static job_t *s_jobs;            /* POD_DOWNLOAD_QUEUE of them, in PSRAM */
static int    s_count;
static bool   s_running;          /* the thread is up */

static void *psram_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(size);
}

void pod_download_path(const pod_episode_t *episode, char *out, size_t cap)
{
    snprintf(out, cap, SAVE_DIR "/%" PRId64 "/%" PRId64 ".mp3", episode->feed_id, episode->id);
}

bool pod_download_saved(const pod_episode_t *episode)
{
    char path[160];
    struct stat st;
    pod_download_path(episode, path, sizeof path);
    return stat(path, &st) == 0 && st.st_size > 0;
}

static job_t *find_locked(int64_t id)
{
    for (int i = 0; i < s_count; i++) {
        if (s_jobs[i].episode.id == id) return &s_jobs[i];
    }
    return NULL;
}

static void remove_locked(job_t *job)
{
    int i = (int)(job - s_jobs);
    memmove(&s_jobs[i], &s_jobs[i + 1], (size_t)(s_count - i - 1) * sizeof s_jobs[0]);
    s_count--;
}

/* Free space on the SD card: -1 when there is no card, or no way to ask. */
static int64_t card_free(void)
{
    const hal_registry_t *reg = hal_get_registry();
    if (!reg) return -1;
    for (uint8_t i = 0; i < reg->storage_count; i++) {
        const hal_storage_driver_t *drv = reg->storage[i];
        if (drv && drv->type == HAL_STORAGE_TYPE_SD && drv->is_mounted && drv->is_mounted()) {
            return drv->get_free_bytes ? (int64_t)drv->get_free_bytes() : INT64_MAX;
        }
    }
    return -1;
}

static void set_job_locked(int64_t id, pod_dl_state_t state, const char *error)
{
    job_t *job = find_locked(id);
    if (!job) return;
    job->state = state;
    if (error) pod_text_copy(job->error, sizeof job->error, error);
}

/* Downloads one episode to its .part file, then renames it. False with
 * `error` set when it could not; `cancel` stops it between chunks. */
static bool download(const pod_episode_t *episode, char *error, size_t error_cap)
{
    char path[160], part[168], dir[128];
    pod_download_path(episode, path, sizeof path);
    snprintf(part, sizeof part, "%s.part", path);
    snprintf(dir, sizeof dir, SAVE_DIR "/%" PRId64, episode->feed_id);
    mkdir(SAVE_DIR, 0755);
    mkdir(dir, 0755);

    uint8_t *chunk = psram_alloc(CHUNK);
    if (!chunk) {
        pod_text_copy(error, error_cap, "Out of memory");
        return false;
    }

    bool ok = false;
    int failures = 0;
    for (;;) {
        /* Whatever an earlier try left is kept, and the rest asked for. */
        struct stat st;
        int64_t have = stat(part, &st) == 0 ? (int64_t)st.st_size : 0;
        int64_t total = 0, start = 0;
        pod_stream_t *stream = pod_net_open(episode->url, have, &total, &start, error, error_cap);
        if (!stream) {
            /* A part the server will not go on from (it may be whole
             * already, or another file's) is started over. */
            if (have > 0 && strcmp(error, POD_NET_PAST_END) == 0) remove(part);
            if (++failures > RETRIES || !pod_net_connected()) break;
            vTaskDelay(pdMS_TO_TICKS(2000 * failures));
            continue;
        }
        if (total > 0) {
            int64_t free_bytes = card_free();
            if (free_bytes >= 0 && free_bytes < total - start + CARD_MARGIN) {
                pod_text_copy(error, error_cap, "Not enough room on the SD card");
                pod_net_close(stream);
                break;
            }
            if (free_bytes < 0) {
                pod_text_copy(error, error_cap, "No SD card");
                pod_net_close(stream);
                break;
            }
        }
        /* The server sent the whole file, or the part is past its end:
         * start the part over. */
        FILE *f = fopen(part, start == have && have > 0 ? "ab" : "wb");
        if (!f) {
            pod_text_copy(error, error_cap, "Could not write to the SD card");
            pod_net_close(stream);
            break;
        }
        int64_t done = start;
        pthread_mutex_lock(&s_lock);
        job_t *job = find_locked(episode->id);
        if (job) {
            job->done = done;
            job->total = total;
        }
        pthread_mutex_unlock(&s_lock);

        bool cancelled = false, broke = false;
        for (;;) {
            int n = pod_net_read(stream, chunk, CHUNK);
            if (n == 0) break;
            if (n < 0) {
                broke = true;
                break;
            }
            if (fwrite(chunk, 1, (size_t)n, f) != (size_t)n) {
                pod_text_copy(error, error_cap, "Could not write to the SD card");
                cancelled = true;     /* not worth a retry */
                break;
            }
            done += n;
            failures = 0;
            pthread_mutex_lock(&s_lock);
            job = find_locked(episode->id);
            if (job) job->done = done;
            cancelled = !job || job->cancel;
            pthread_mutex_unlock(&s_lock);
            if (cancelled) break;
        }
        bool closed = fclose(f) == 0;
        pod_net_close(stream);
        if (cancelled) break;
        if (broke || (total > 0 && done < total)) {
            pod_text_copy(error, error_cap, "The download was cut off");
            if (++failures > RETRIES) break;
            vTaskDelay(pdMS_TO_TICKS(2000 * failures));
            continue;
        }
        if (!closed) {
            pod_text_copy(error, error_cap, "Could not write to the SD card");
            break;
        }
        remove(path);
        ok = rename(part, path) == 0;
        if (!ok) pod_text_copy(error, error_cap, "Could not write to the SD card");
        else ESP_LOGI(TAG, "saved %" PRId64 ", %" PRId64 " bytes", episode->id, done);
        break;
    }
    free(chunk);
    return ok;
}

static void download_task(void *arg)
{
    (void)arg;
    uint32_t awake = power_manager_hold("a podcast is downloading");
    pod_episode_t *episode = psram_alloc(sizeof *episode);
    char error[64];

    while (episode) {
        pthread_mutex_lock(&s_lock);
        job_t *next = NULL;
        for (int i = 0; i < s_count && !next; i++) {
            if (s_jobs[i].state == POD_DL_QUEUED) next = &s_jobs[i];
        }
        if (!next) {
            s_running = false;
            pthread_mutex_unlock(&s_lock);
            break;
        }
        next->state = POD_DL_RUNNING;
        next->error[0] = '\0';
        *episode = next->episode;
        pthread_mutex_unlock(&s_lock);

        error[0] = '\0';
        bool ok = download(episode, error, sizeof error);

        pthread_mutex_lock(&s_lock);
        job_t *job = find_locked(episode->id);
        bool cancelled = !job || job->cancel;
        if (ok && !cancelled) {
            remove_locked(job);
        } else if (cancelled) {
            if (job) remove_locked(job);
        } else {
            set_job_locked(episode->id, POD_DL_FAILED, error[0] ? error : "The download failed");
        }
        pthread_mutex_unlock(&s_lock);

        if (ok && !cancelled) {
            pod_lib_save_episode(episode, (int64_t)time(NULL));
            pod_lib_flush();
        } else {
            if (cancelled) {
                char path[168];
                pod_download_path(episode, path, sizeof path - 5);
                strcat(path, ".part");
                remove(path);
            }
            ESP_LOGW(TAG, "%" PRId64 ": %s", episode->id, cancelled ? "cancelled" : error);
        }
    }
    free(episode);
    power_manager_release(awake);
#ifndef SIMULATOR_BUILD
    ESP_LOGI(TAG, "done; %u bytes of stack to spare", (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
    vTaskDelete(NULL);
}

const char *pod_download_start(const pod_episode_t *episode)
{
    if (!episode->url[0]) return "This episode has no audio";
    if (!pod_episode_playable(episode)) return "Only MP3 episodes can be saved";
    if (pod_download_saved(episode)) {
        pod_lib_save_episode(episode, (int64_t)time(NULL));
        pod_lib_flush();
        return NULL;
    }
    if (card_free() < 0) return "No SD card";
    if (!pod_net_connected()) return "No network";

    pthread_mutex_lock(&s_lock);
    if (!s_jobs) s_jobs = psram_alloc(POD_DOWNLOAD_QUEUE * sizeof *s_jobs);
    const char *why = NULL;
    job_t *job = s_jobs ? find_locked(episode->id) : NULL;
    if (!s_jobs) {
        why = "Out of memory";
    } else if (job && job->state != POD_DL_FAILED) {
        /* Queued or running already. */
    } else {
        if (!job) {
            if (s_count == POD_DOWNLOAD_QUEUE) {
                /* Make room by forgetting a failed one, if there is one. */
                for (int i = 0; i < s_count && !job; i++) {
                    if (s_jobs[i].state == POD_DL_FAILED) {
                        remove_locked(&s_jobs[i]);
                        break;
                    }
                }
                if (s_count == POD_DOWNLOAD_QUEUE) why = "Too many downloads at once";
            }
            if (!why) {
                job = &s_jobs[s_count++];
                memset(job, 0, sizeof *job);
                job->episode = *episode;
            }
        }
        if (job && !why) {
            job->state = POD_DL_QUEUED;
            job->cancel = false;
            job->error[0] = '\0';
            if (!s_running) {
                s_running = xTaskCreate(download_task, "pod_download", TASK_STACK, NULL,
                                        TASK_PRIORITY, NULL) == pdPASS;
                if (!s_running) {
                    remove_locked(job);
                    why = "Not enough memory to download";
                }
            }
        }
    }
    pthread_mutex_unlock(&s_lock);
    return why;
}

void pod_download_cancel(int64_t episode_id)
{
    pthread_mutex_lock(&s_lock);
    job_t *job = s_jobs ? find_locked(episode_id) : NULL;
    if (job) {
        if (job->state == POD_DL_RUNNING) job->cancel = true;   /* the thread removes it */
        else remove_locked(job);
    }
    pthread_mutex_unlock(&s_lock);
}

bool pod_download_delete(const pod_episode_t *episode)
{
    pod_download_cancel(episode->id);
    char path[168];
    pod_download_path(episode, path, sizeof path);
    bool ok = remove(path) == 0 || access(path, F_OK) != 0;
    pod_lib_save_episode(episode, 0);
    pod_lib_flush();
    return ok;
}

void pod_download_status(const pod_episode_t *episode, pod_dl_status_t *out)
{
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&s_lock);
    const job_t *job = s_jobs ? find_locked(episode->id) : NULL;
    if (job) {
        out->state = job->state;
        out->done = job->done;
        out->total = job->total > 0 ? job->total : job->episode.bytes;
        pod_text_copy(out->error, sizeof out->error, job->error);
    }
    pthread_mutex_unlock(&s_lock);
    if (!job && pod_download_saved(episode)) out->state = POD_DL_SAVED;
}

int pod_download_active(void)
{
    pthread_mutex_lock(&s_lock);
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        if (s_jobs[i].state == POD_DL_QUEUED || s_jobs[i].state == POD_DL_RUNNING) n++;
    }
    pthread_mutex_unlock(&s_lock);
    return n;
}
