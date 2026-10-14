/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — PodcastIndex: the requests the app makes and what it keeps of
 * the answers.
 *
 * Nothing here touches the network: the paths it builds are sent by
 * pod_net.c, and the answers it parses come back from there. See
 * https://podcastindex-org.github.io/docs-api/ for the API.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POD_API_BASE "https://api.podcastindex.org/api/1.0"
#define POD_USER_AGENT "ThistleOS-Podcasts/0.1"

#define POD_TITLE_MAX   160
#define POD_AUTHOR_MAX  96
#define POD_CATS_MAX    96
#define POD_DESC_MAX    1024
#define POD_URL_MAX     768
#define POD_TYPE_MAX    32
#define POD_LANG_MAX    16

/* A podcast, as PodcastIndex calls a feed. */
typedef struct {
    int64_t id;
    char    title[POD_TITLE_MAX];
    char    author[POD_AUTHOR_MAX];
    char    categories[POD_CATS_MAX];  /* "News, Comedy" */
    char    language[POD_LANG_MAX];
    char    description[POD_DESC_MAX]; /* plain text */
    int32_t episode_count;             /* 0 when not known */
    int64_t newest;                    /* its newest episode's date; 0 when not known */
} pod_feed_t;

typedef struct {
    int64_t id;                        /* past 2^32 */
    int64_t feed_id;
    char    title[POD_TITLE_MAX];
    char    feed_title[POD_TITLE_MAX]; /* "" when the answer did not say */
    char    url[POD_URL_MAX];          /* the audio: the enclosure */
    char    type[POD_TYPE_MAX];        /* "audio/mpeg" */
    int64_t bytes;                     /* the enclosure's length; 0 when not known */
    int32_t duration;                  /* seconds; 0 when not known */
    int64_t published;
    char    description[POD_DESC_MAX]; /* plain text */
} pod_episode_t;

typedef struct {
    int32_t id;
    char    name[48];
} pod_category_t;

typedef enum {
    POD_SEARCH_TERM,    /* /search/byterm: titles, authors, owners */
    POD_SEARCH_TITLE,   /* /search/bytitle: titles alone */
    POD_SEARCH_PERSON,  /* /search/byperson: episodes a person is in */
    POD_SEARCH_MUSIC,   /* /search/music/byterm: music feeds */
    POD_SEARCH_MODES
} pod_search_mode_t;

/* "Podcasts", "Titles", "People", "Music". */
const char *pod_search_mode_name(pod_search_mode_t mode);

/* Whether a mode's answer is episodes rather than podcasts. */
bool pod_search_finds_episodes(pod_search_mode_t mode);

/* ------------------------------------------------------------------ */
/* Requests: a path and query under POD_API_BASE                        */
/* ------------------------------------------------------------------ */

/* Each writes the path into `out` and returns false if it does not fit. */
bool pod_api_search_path(char *out, size_t cap, pod_search_mode_t mode, const char *query,
                         bool clean, int max);
/* `category` is a name or an id, "" for all; `lang` likewise, "" for any. */
bool pod_api_trending_path(char *out, size_t cap, const char *category, const char *lang, int max);
bool pod_api_categories_path(char *out, size_t cap);
bool pod_api_feed_path(char *out, size_t cap, int64_t feed_id);
/* The newest episodes of up to 200 podcasts at once, newest first. */
bool pod_api_episodes_path(char *out, size_t cap, const int64_t *feed_ids, int feed_count, int max);
bool pod_api_episode_path(char *out, size_t cap, int64_t episode_id);

/* The X-Auth-Date and Authorization headers for a request made at `now`. */
void pod_api_auth(const char *key, const char *secret, int64_t now, char date[24], char auth[41]);

/* ------------------------------------------------------------------ */
/* Answers                                                              */
/* ------------------------------------------------------------------ */

/* Each parses the JSON PodcastIndex answered with and returns how many it
 * read into `out`, up to `max`, or -1 when the answer is not one, or says it
 * failed, with what it said (or "Unexpected answer") in `error`. */

/* "feeds", or the one "feed" /podcasts/byfeedid answers with. */
int pod_api_parse_feeds(const char *json, size_t len, pod_feed_t *out, int max,
                        char *error, size_t error_cap);
/* "items", or the one "episode" /episodes/byid answers with. */
int pod_api_parse_episodes(const char *json, size_t len, pod_episode_t *out, int max,
                           char *error, size_t error_cap);
/* /categories/list, whose list is also called "feeds". */
int pod_api_parse_categories(const char *json, size_t len, pod_category_t *out, int max,
                             char *error, size_t error_cap);

/* Whether the app can play an episode: MP3. Judged by its type, or by its
 * URL's extension when the type says nothing useful. */
bool pod_episode_playable(const pod_episode_t *episode);
