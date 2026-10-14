/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the podcasts and episodes the app remembers.
 *
 * Starred, viewed, played (with where it was left) and saved to the SD card:
 * each podcast and episode is kept once, with what the lists show of it, so
 * they can be shown with no network. A podcast is kept while starred or
 * among the POD_LIB_RECENT viewed last; an episode while starred, saved,
 * among the POD_LIB_RECENT viewed last or the POD_LIB_HISTORY played last.
 *
 * One file, <dir>/library.json, read once and kept in PSRAM; every call
 * works on that copy and may come from any task. A change is written by
 * pod_lib_flush(), which the app calls when it has finished a change, and
 * the player every so often while it plays.
 */
#pragma once

#include "podcasts/pod_api.h"

#include <stdbool.h>
#include <stdint.h>

#define POD_LIB_FEEDS_MAX     64
#define POD_LIB_EPISODES_MAX 160
#define POD_LIB_RECENT        20
#define POD_LIB_HISTORY       30
/* A description is kept this long, so the file stays small. */
#define POD_LIB_DESC_MAX     400

typedef struct {
    pod_feed_t feed;
    int64_t    starred_at;   /* 0: not starred */
    int64_t    viewed_at;    /* 0: never opened */
} pod_lib_feed_t;

typedef struct {
    pod_episode_t episode;
    int64_t starred_at;      /* 0: not starred */
    int64_t viewed_at;       /* 0: never opened */
    int64_t played_at;       /* 0: never played */
    int64_t saved_at;        /* 0: not on the SD card */
    int32_t position;        /* seconds in, where it was left */
    bool    finished;        /* played to the end */
} pod_lib_episode_t;

typedef enum {
    POD_LIB_STARRED,         /* podcasts by title; episodes starred last first */
    POD_LIB_VIEWED,          /* opened last first */
    POD_LIB_SAVED,           /* episodes saved last first */
    POD_LIB_CONTINUE,        /* episodes begun and not finished, played last first */
} pod_lib_list_t;

/* Reads <dir>/library.json, once: later calls with the same `dir` do
 * nothing. False when there was none, or it could not be read, which leaves
 * an empty library to fill. */
bool pod_lib_open(const char *dir);

/* Writes the library if it changed since it was read or last written: true
 * when it is on the card. */
bool pod_lib_flush(void);

/* Forgets the library read, unwritten changes included. For tests. */
void pod_lib_close(void);

/* ------------------------------------------------------------------ */
/* Podcasts                                                             */
/* ------------------------------------------------------------------ */

void pod_lib_star_feed(const pod_feed_t *feed, bool starred, int64_t now);
void pod_lib_view_feed(const pod_feed_t *feed, int64_t now);
bool pod_lib_feed(int64_t id, pod_lib_feed_t *out);
bool pod_lib_feed_starred(int64_t id);

/* POD_LIB_STARRED or POD_LIB_VIEWED. How many were written, up to max. */
int pod_lib_feeds(pod_lib_list_t which, pod_feed_t *out, int max);

/* How many a list has. */
int pod_lib_count_feeds(pod_lib_list_t which);
int pod_lib_count_episodes(pod_lib_list_t which);

/* The starred podcasts' ids, for asking for their newest episodes. */
int pod_lib_starred_feed_ids(int64_t *out, int max);

/* ------------------------------------------------------------------ */
/* Episodes                                                             */
/* ------------------------------------------------------------------ */

void pod_lib_star_episode(const pod_episode_t *episode, bool starred, int64_t now);
void pod_lib_view_episode(const pod_episode_t *episode, int64_t now);
/* Where it was left, and whether that was the end. */
void pod_lib_play_episode(const pod_episode_t *episode, int32_t position, bool finished, int64_t now);
/* `saved_at` 0 when it is no longer on the card. */
void pod_lib_save_episode(const pod_episode_t *episode, int64_t saved_at);

/* What is known of one; false when nothing is, with `out` zeroed but for
 * the id. */
bool pod_lib_episode(int64_t id, pod_lib_episode_t *out);

/* Any of the four lists. How many were written, up to max. */
int pod_lib_episodes(pod_lib_list_t which, pod_lib_episode_t *out, int max);

/* Every episode of one podcast the library knows, newest first: what a
 * podcast's page shows with no network. */
int pod_lib_feed_episodes(int64_t feed_id, pod_lib_episode_t *out, int max);
