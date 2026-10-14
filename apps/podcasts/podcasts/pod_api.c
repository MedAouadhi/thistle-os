/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — PodcastIndex requests and answers.
 */
#include "podcasts/pod_api.h"
#include "podcasts/pod_json.h"
#include "podcasts/pod_sha1.h"
#include "podcasts/pod_text.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* A description as the feed wrote it, before its HTML is taken out. Over
 * 4 KB, so on the device it is allocated in PSRAM (CLAUDE.md). */
#define RAW_TEXT_MAX 6144

const char *pod_search_mode_name(pod_search_mode_t mode)
{
    switch (mode) {
    case POD_SEARCH_TITLE:  return "Titles";
    case POD_SEARCH_PERSON: return "People";
    case POD_SEARCH_MUSIC:  return "Music";
    default:                return "Podcasts";
    }
}

bool pod_search_finds_episodes(pod_search_mode_t mode)
{
    return mode == POD_SEARCH_PERSON;
}

/* ------------------------------------------------------------------ */
/* Requests                                                             */
/* ------------------------------------------------------------------ */

static bool fits(int n, size_t cap)
{
    return n >= 0 && (size_t)n < cap;
}

bool pod_api_search_path(char *out, size_t cap, pod_search_mode_t mode, const char *query,
                         bool clean, int max)
{
    char q[256];
    if (!pod_text_url_encode(query ? query : "", q, sizeof q)) return false;
    const char *path = mode == POD_SEARCH_TITLE  ? "/search/bytitle"
                     : mode == POD_SEARCH_PERSON ? "/search/byperson"
                     : mode == POD_SEARCH_MUSIC  ? "/search/music/byterm"
                                                 : "/search/byterm";
    /* byperson has no clean; bytitle finds more with similar. */
    bool with_clean = clean && mode != POD_SEARCH_PERSON;
    int n = snprintf(out, cap, "%s?q=%s&max=%d%s%s", path, q, max,
                     with_clean ? "&clean" : "", mode == POD_SEARCH_TITLE ? "&similar" : "");
    return fits(n, cap);
}

bool pod_api_trending_path(char *out, size_t cap, const char *category, const char *lang, int max)
{
    char cat[96] = "", lng[48] = "";
    if (category && *category && !pod_text_url_encode(category, cat, sizeof cat)) return false;
    if (lang && *lang && !pod_text_url_encode(lang, lng, sizeof lng)) return false;
    int n = snprintf(out, cap, "/podcasts/trending?max=%d%s%s%s%s", max, *cat ? "&cat=" : "", cat,
                     *lng ? "&lang=" : "", lng);
    return fits(n, cap);
}

bool pod_api_categories_path(char *out, size_t cap)
{
    return fits(snprintf(out, cap, "/categories/list"), cap);
}

bool pod_api_feed_path(char *out, size_t cap, int64_t feed_id)
{
    return fits(snprintf(out, cap, "/podcasts/byfeedid?id=%" PRId64, feed_id), cap);
}

bool pod_api_episodes_path(char *out, size_t cap, const int64_t *feed_ids, int feed_count, int max)
{
    if (feed_count <= 0) return false;
    if (feed_count > 200) feed_count = 200;  /* the API's limit */
    int n = snprintf(out, cap, "/episodes/byfeedid?id=");
    if (!fits(n, cap)) return false;
    size_t len = (size_t)n;
    for (int i = 0; i < feed_count; i++) {
        n = snprintf(out + len, cap - len, "%s%" PRId64, i ? "," : "", feed_ids[i]);
        if (!fits(n, cap - len)) return false;
        len += (size_t)n;
    }
    n = snprintf(out + len, cap - len, "&max=%d", max);
    return fits(n, cap - len);
}

bool pod_api_episode_path(char *out, size_t cap, int64_t episode_id)
{
    return fits(snprintf(out, cap, "/episodes/byid?id=%" PRId64, episode_id), cap);
}

void pod_api_auth(const char *key, const char *secret, int64_t now, char date[24], char auth[41])
{
    snprintf(date, 24, "%" PRId64, now);
    char joined[256];
    snprintf(joined, sizeof joined, "%s%s%s", key, secret, date);
    pod_sha1_hex(joined, auth);
    /* The secret was in it. */
    memset(joined, 0, sizeof joined);
}

/* ------------------------------------------------------------------ */
/* Answers                                                              */
/* ------------------------------------------------------------------ */

static void set_error(char *error, size_t cap, const char *text)
{
    if (error && cap) pod_text_copy(error, cap, text);
}

/* A description, out of its HTML. */
static void read_description(pod_json_t *j, char *raw, char *out, size_t cap)
{
    if (!raw) {
        pod_json_skip(j);
        return;
    }
    pod_json_string(j, raw, RAW_TEXT_MAX);
    pod_text_from_html(raw, out, cap);
}

/* A title, which feeds write in HTML often enough ("Q&amp;A") to read it as
 * such too. */
static void read_title(pod_json_t *j, char *raw, char *out, size_t cap)
{
    if (!raw) {
        pod_json_string(j, out, cap);
        return;
    }
    pod_json_string(j, raw, RAW_TEXT_MAX);
    pod_text_from_html(raw, out, cap);
}

/* "categories": {"104": "Tv", "105": "Film"} as "Tv, Film". */
static void read_categories(pod_json_t *j, char *out, size_t cap)
{
    out[0] = '\0';
    if (!pod_json_object(j)) return;
    char key[16], name[48];
    size_t len = 0;
    while (pod_json_key(j, key, sizeof key)) {
        pod_json_string(j, name, sizeof name);
        size_t n = strlen(name);
        if (!n || len + n + 3 > cap) continue;
        if (len) {
            memcpy(out + len, ", ", 2);
            len += 2;
        }
        memcpy(out + len, name, n + 1);
        len += n;
    }
}

static void read_feed(pod_json_t *j, pod_feed_t *f, char *raw)
{
    memset(f, 0, sizeof *f);
    char key[32];
    int64_t v;
    if (!pod_json_object(j)) return;
    while (pod_json_key(j, key, sizeof key)) {
        if (strcmp(key, "id") == 0) {
            pod_json_int(j, &f->id);
        } else if (strcmp(key, "title") == 0) {
            read_title(j, raw, f->title, sizeof f->title);
        } else if (strcmp(key, "author") == 0) {
            pod_json_string(j, f->author, sizeof f->author);
        } else if (strcmp(key, "ownerName") == 0 && !f->author[0]) {
            pod_json_string(j, f->author, sizeof f->author);
        } else if (strcmp(key, "description") == 0) {
            read_description(j, raw, f->description, sizeof f->description);
        } else if (strcmp(key, "language") == 0) {
            pod_json_string(j, f->language, sizeof f->language);
        } else if (strcmp(key, "categories") == 0) {
            read_categories(j, f->categories, sizeof f->categories);
        } else if (strcmp(key, "episodeCount") == 0) {
            if (pod_json_int(j, &v)) f->episode_count = (int32_t)v;
        } else if (strcmp(key, "newestItemPubdate") == 0 ||
                   strcmp(key, "newestItemPublishTime") == 0) {
            if (pod_json_int(j, &v) && v > f->newest) f->newest = v;
        } else {
            pod_json_skip(j);
        }
    }
}

static void read_episode(pod_json_t *j, pod_episode_t *e, char *raw)
{
    memset(e, 0, sizeof *e);
    char key[32];
    int64_t v;
    if (!pod_json_object(j)) return;
    while (pod_json_key(j, key, sizeof key)) {
        if (strcmp(key, "id") == 0) {
            pod_json_int(j, &e->id);
        } else if (strcmp(key, "feedId") == 0) {
            pod_json_int(j, &e->feed_id);
        } else if (strcmp(key, "title") == 0) {
            read_title(j, raw, e->title, sizeof e->title);
        } else if (strcmp(key, "feedTitle") == 0) {
            read_title(j, raw, e->feed_title, sizeof e->feed_title);
        } else if (strcmp(key, "enclosureUrl") == 0) {
            pod_json_string(j, e->url, sizeof e->url);
        } else if (strcmp(key, "enclosureType") == 0) {
            pod_json_string(j, e->type, sizeof e->type);
        } else if (strcmp(key, "enclosureLength") == 0) {
            if (pod_json_int(j, &v) && v > 0) e->bytes = v;
        } else if (strcmp(key, "duration") == 0) {
            if (pod_json_int(j, &v) && v > 0 && v < INT32_MAX) e->duration = (int32_t)v;
        } else if (strcmp(key, "datePublished") == 0) {
            pod_json_int(j, &e->published);
        } else if (strcmp(key, "description") == 0) {
            read_description(j, raw, e->description, sizeof e->description);
        } else {
            pod_json_skip(j);
        }
    }
    /* An enclosure URL longer than there is room for would be cut short
     * into another URL; better none. */
    if (strlen(e->url) >= sizeof e->url - 1) e->url[0] = '\0';
}

typedef enum { LIST_FEEDS, LIST_EPISODES, LIST_CATEGORIES } list_kind_t;

/* Walks an answer's top level: its list (or the one item) into `out`, and
 * its status and description into `error` when it failed. */
static int parse(const char *json, size_t len, list_kind_t kind, void *out, int max,
                 char *error, size_t error_cap)
{
    set_error(error, error_cap, "");
    char *raw = malloc(RAW_TEXT_MAX);
    pod_json_t j;
    pod_json_init(&j, json, len);

    const char *list_key = kind == LIST_EPISODES ? "items" : "feeds";
    const char *one_key  = kind == LIST_EPISODES ? "episode" : kind == LIST_FEEDS ? "feed" : NULL;
    int count = 0;
    bool found = false;
    bool failed = false;
    char key[32], status[16], description[128] = "";

    if (!pod_json_object(&j)) {
        free(raw);
        set_error(error, error_cap, "Unexpected answer");
        return -1;
    }
    while (pod_json_key(&j, key, sizeof key)) {
        if (strcmp(key, list_key) == 0) {
            if (!pod_json_array(&j)) continue;
            found = true;
            while (pod_json_next(&j)) {
                if (count >= max) {
                    pod_json_skip(&j);
                    continue;
                }
                if (kind == LIST_FEEDS) {
                    read_feed(&j, (pod_feed_t *)out + count, raw);
                    if (((pod_feed_t *)out)[count].id) count++;
                } else if (kind == LIST_EPISODES) {
                    read_episode(&j, (pod_episode_t *)out + count, raw);
                    if (((pod_episode_t *)out)[count].id) count++;
                } else {
                    pod_category_t *c = (pod_category_t *)out + count;
                    memset(c, 0, sizeof *c);
                    if (pod_json_object(&j)) {
                        int64_t v;
                        while (pod_json_key(&j, key, sizeof key)) {
                            if (strcmp(key, "id") == 0 && pod_json_int(&j, &v)) c->id = (int32_t)v;
                            else if (strcmp(key, "name") == 0) pod_json_string(&j, c->name, sizeof c->name);
                            else pod_json_skip(&j);
                        }
                    }
                    if (c->id && c->name[0]) count++;
                }
            }
        } else if (one_key && strcmp(key, one_key) == 0) {
            /* An id that was not found answers with "feed": [] or an empty
             * object: zero results, not an error. */
            found = true;
            if (count < max) {
                if (kind == LIST_FEEDS) {
                    read_feed(&j, (pod_feed_t *)out, raw);
                    if (((pod_feed_t *)out)->id) count = 1;
                } else {
                    read_episode(&j, (pod_episode_t *)out, raw);
                    if (((pod_episode_t *)out)->id) count = 1;
                }
            } else {
                pod_json_skip(&j);
            }
        } else if (strcmp(key, "status") == 0) {
            pod_json_string(&j, status, sizeof status);
            failed = strcmp(status, "false") == 0;
        } else if (strcmp(key, "description") == 0) {
            pod_json_string(&j, description, sizeof description);
        } else {
            pod_json_skip(&j);
        }
    }
    free(raw);

    if (j.error) {
        set_error(error, error_cap, "Unexpected answer");
        return -1;
    }
    if (failed || !found) {
        set_error(error, error_cap, failed && description[0] ? description : "Unexpected answer");
        return -1;
    }
    return count;
}

int pod_api_parse_feeds(const char *json, size_t len, pod_feed_t *out, int max,
                        char *error, size_t error_cap)
{
    return parse(json, len, LIST_FEEDS, out, max, error, error_cap);
}

int pod_api_parse_episodes(const char *json, size_t len, pod_episode_t *out, int max,
                           char *error, size_t error_cap)
{
    return parse(json, len, LIST_EPISODES, out, max, error, error_cap);
}

int pod_api_parse_categories(const char *json, size_t len, pod_category_t *out, int max,
                             char *error, size_t error_cap)
{
    return parse(json, len, LIST_CATEGORIES, out, max, error, error_cap);
}

bool pod_episode_playable(const pod_episode_t *e)
{
    if (!e->url[0]) return false;
    const char *t = e->type;
    if (strcasecmp(t, "audio/mpeg") == 0 || strcasecmp(t, "audio/mp3") == 0 ||
        strcasecmp(t, "audio/mpeg3") == 0 || strcasecmp(t, "audio/x-mpeg") == 0 ||
        strcasecmp(t, "audio/x-mp3") == 0) {
        return true;
    }
    /* Feeds that leave the type out, or write "application/octet-stream":
     * the URL's extension, before any query, says. */
    if (t[0] && strncasecmp(t, "application/", 12) != 0 && strcasecmp(t, "binary/octet-stream") != 0) {
        return false;
    }
    const char *end = strpbrk(e->url, "?#");
    size_t n = end ? (size_t)(end - e->url) : strlen(e->url);
    return n >= 4 && strncasecmp(e->url + n - 4, ".mp3", 4) == 0;
}
