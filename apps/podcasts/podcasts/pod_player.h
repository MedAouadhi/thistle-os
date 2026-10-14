/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — plays an episode, streamed or from the SD card.
 *
 * The app's pages come and go (the app manager destroys an app whenever
 * another comes forward); the player does not. It lives in this module's
 * statics and two threads of its own: one fetches the MP3, over HTTP or from
 * the card, into a ring in PSRAM; the other decodes it with minimp3 and
 * writes the PCM to the audio HAL. Every call here returns at once and may
 * come from any task.
 *
 * Paused for 10 minutes, it lets its threads and connection go, and picks up
 * where it was when resumed. Where each episode was left goes to the
 * library: when it starts, every 30 s, on a pause and at the end.
 */
#pragma once

#include "podcasts/pod_api.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    POD_PLAYER_IDLE,      /* nothing loaded */
    POD_PLAYER_LOADING,   /* connecting, or buffering before the first sound */
    POD_PLAYER_PLAYING,
    POD_PLAYER_PAUSED,
    POD_PLAYER_ENDED,     /* played to the end */
    POD_PLAYER_FAILED,    /* could not go on; `error` says why */
} pod_player_state_t;

typedef struct {
    pod_player_state_t state;
    int64_t episode_id;   /* 0 when idle */
    int32_t position;     /* seconds in */
    int32_t duration;     /* seconds; 0 when not known */
    bool    buffering;    /* playing, but waiting for the network */
    bool    local;        /* from the SD card */
    char    error[80];
} pod_player_status_t;

/* Plays `episode` from `from` seconds, or from where the library says it
 * was left when `from` is negative. NULL when it is starting; otherwise what
 * to tell the user. */
const char *pod_player_play(const pod_episode_t *episode, int32_t from);

void pod_player_pause(void);
void pod_player_resume(void);
/* Pauses what is playing, or plays what is paused or has ended. */
void pod_player_toggle(void);
void pod_player_seek(int32_t position);
void pod_player_skip(int32_t seconds);
/* Stops and unloads, saving where it was. */
void pod_player_stop(void);

void pod_player_status(pod_player_status_t *out);
/* The episode loaded: false when none. */
bool pod_player_episode(pod_episode_t *out);
/* Whether something is loaded and not stopped: playing, paused, loading. */
bool pod_player_active(void);
