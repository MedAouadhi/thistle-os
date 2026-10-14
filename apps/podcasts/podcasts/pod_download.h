/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — episodes saved to the SD card, to play with no network.
 *
 * One download at a time, on a thread of its own, from a queue: the file
 * grows as <id>.mp3.part beside where it goes, is picked up where it was
 * left if cut short (a Range request), and is renamed once whole, when the
 * library records it as saved. Like the player, it outlives the app's
 * pages: leaving Podcasts does not stop a download.
 */
#pragma once

#include "podcasts/pod_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POD_DOWNLOAD_QUEUE 16

typedef enum {
    POD_DL_NONE,          /* neither saved nor on its way */
    POD_DL_QUEUED,
    POD_DL_RUNNING,
    POD_DL_FAILED,        /* the last try failed; `error` says why */
    POD_DL_SAVED,         /* the file is on the card */
} pod_dl_state_t;

typedef struct {
    pod_dl_state_t state;
    int64_t        done;     /* bytes so far */
    int64_t        total;    /* 0 when not known */
    char           error[64];
} pod_dl_status_t;

/* Where an episode is saved: <card>/podcasts/<feed id>/<episode id>.mp3. */
void pod_download_path(const pod_episode_t *episode, char *out, size_t cap);

/* Whether the episode's file is on the card. */
bool pod_download_saved(const pod_episode_t *episode);

/* Queues a download. NULL when it was queued (or is running or saved
 * already); otherwise what to tell the user. */
const char *pod_download_start(const pod_episode_t *episode);

/* Stops a queued or running download and deletes what it had written. */
void pod_download_cancel(int64_t episode_id);

/* Deletes a saved episode's file. */
bool pod_download_delete(const pod_episode_t *episode);

void pod_download_status(const pod_episode_t *episode, pod_dl_status_t *out);

/* How many are queued or running. */
int pod_download_active(void);
