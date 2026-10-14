/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the podcasts and episodes the app remembers.
 */
#include "podcasts/pod_library.h"
#include "podcasts/pod_json.h"
#include "podcasts/pod_text.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define FILE_NAME   "library.json"
/* The library at its fullest is some 400 KB; a file far past that is not
 * one this app wrote. */
#define FILE_MAX    (2 * 1024 * 1024)

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
    bool               open;
    char               dir[160];
    bool               dirty;
    pod_lib_feed_t    *feeds;      /* POD_LIB_FEEDS_MAX of them, in PSRAM */
    int                feed_count;
    pod_lib_episode_t *episodes;   /* POD_LIB_EPISODES_MAX */
    int                episode_count;
} s_lib;

/* ------------------------------------------------------------------ */
/* Keeping and dropping                                                 */
/* ------------------------------------------------------------------ */

static int cmp_desc(int64_t a, int64_t b)
{
    return a < b ? 1 : a > b ? -1 : 0;
}
/* Where `v` ranks, from 0, among the podcasts' viewed_at. */
static int feed_rank(int64_t v)
{
    int r = 0;
    for (int i = 0; i < s_lib.feed_count; i++) {
        if (s_lib.feeds[i].viewed_at > v) r++;
    }
    return r;
}

/* Where `v` ranks among the episodes' viewed_at, or their played_at. */
static int episode_rank(int64_t v, bool played)
{
    int r = 0;
    for (int i = 0; i < s_lib.episode_count; i++) {
        const pod_lib_episode_t *e = &s_lib.episodes[i];
        if ((played ? e->played_at : e->viewed_at) > v) r++;
    }
    return r;
}

static bool feed_kept(const pod_lib_feed_t *f)
{
    return f->starred_at || (f->viewed_at && feed_rank(f->viewed_at) < POD_LIB_RECENT);
}

static bool episode_kept(const pod_lib_episode_t *e)
{
    if (e->starred_at || e->saved_at) return true;
    if (e->viewed_at && episode_rank(e->viewed_at, false) < POD_LIB_RECENT) return true;
    return e->played_at && episode_rank(e->played_at, true) < POD_LIB_HISTORY;
}

/* Drops what nothing keeps any more. Which to keep is settled before any
 * goes, since the ranks are among all of them. */
static void prune(void)
{
    bool keep[POD_LIB_EPISODES_MAX];
    for (int i = 0; i < s_lib.feed_count; i++) keep[i] = feed_kept(&s_lib.feeds[i]);
    int kept = 0;
    for (int i = 0; i < s_lib.feed_count; i++) {
        if (keep[i]) s_lib.feeds[kept++] = s_lib.feeds[i];
    }
    s_lib.feed_count = kept;

    for (int i = 0; i < s_lib.episode_count; i++) keep[i] = episode_kept(&s_lib.episodes[i]);
    kept = 0;
    for (int i = 0; i < s_lib.episode_count; i++) {
        if (keep[i]) s_lib.episodes[kept++] = s_lib.episodes[i];
    }
    s_lib.episode_count = kept;
}

static bool ensure_arrays(void)
{
    if (!s_lib.feeds) s_lib.feeds = calloc(POD_LIB_FEEDS_MAX, sizeof *s_lib.feeds);
    if (!s_lib.episodes) s_lib.episodes = calloc(POD_LIB_EPISODES_MAX, sizeof *s_lib.episodes);
    return s_lib.feeds && s_lib.episodes;
}

static void copy_feed(pod_feed_t *dst, const pod_feed_t *src)
{
    *dst = *src;
    pod_text_copy(dst->description, POD_LIB_DESC_MAX, src->description);
}

static void copy_episode(pod_episode_t *dst, const pod_episode_t *src)
{
    /* What one list knew and another did not: the podcast's title. */
    char feed_title[POD_TITLE_MAX];
    pod_text_copy(feed_title, sizeof feed_title, dst->id == src->id ? dst->feed_title : "");
    *dst = *src;
    if (!dst->feed_title[0]) pod_text_copy(dst->feed_title, sizeof dst->feed_title, feed_title);
    pod_text_copy(dst->description, POD_LIB_DESC_MAX, src->description);
}

/* The entry for `feed`, made if need be: when the library is full, the one
 * kept least is let go first. NULL only when out of memory. */
static pod_lib_feed_t *feed_entry(const pod_feed_t *feed)
{
    if (!ensure_arrays()) return NULL;
    for (int i = 0; i < s_lib.feed_count; i++) {
        if (s_lib.feeds[i].feed.id == feed->id) {
            copy_feed(&s_lib.feeds[i].feed, feed);
            return &s_lib.feeds[i];
        }
    }
    if (s_lib.feed_count == POD_LIB_FEEDS_MAX) {
        prune();
        if (s_lib.feed_count == POD_LIB_FEEDS_MAX) {
            /* All starred: the one viewed longest ago goes. */
            int oldest = 0;
            for (int i = 1; i < s_lib.feed_count; i++) {
                if (s_lib.feeds[i].starred_at < s_lib.feeds[oldest].starred_at) oldest = i;
            }
            s_lib.feeds[oldest] = s_lib.feeds[--s_lib.feed_count];
        }
    }
    pod_lib_feed_t *f = &s_lib.feeds[s_lib.feed_count++];
    memset(f, 0, sizeof *f);
    copy_feed(&f->feed, feed);
    return f;
}

static pod_lib_episode_t *find_episode(int64_t id)
{
    for (int i = 0; i < s_lib.episode_count; i++) {
        if (s_lib.episodes[i].episode.id == id) return &s_lib.episodes[i];
    }
    return NULL;
}

static pod_lib_episode_t *episode_entry(const pod_episode_t *episode)
{
    if (!ensure_arrays()) return NULL;
    pod_lib_episode_t *e = find_episode(episode->id);
    if (e) {
        copy_episode(&e->episode, episode);
        return e;
    }
    if (s_lib.episode_count == POD_LIB_EPISODES_MAX) {
        prune();
        if (s_lib.episode_count == POD_LIB_EPISODES_MAX) {
            /* Everything starred or saved: the starred one that is not saved
             * and was starred longest ago goes; a saved file is never
             * forgotten. */
            int victim = -1;
            for (int i = 0; i < s_lib.episode_count; i++) {
                const pod_lib_episode_t *c = &s_lib.episodes[i];
                if (c->saved_at) continue;
                if (victim < 0 || c->starred_at < s_lib.episodes[victim].starred_at) victim = i;
            }
            if (victim < 0) return NULL;
            s_lib.episodes[victim] = s_lib.episodes[--s_lib.episode_count];
        }
    }
    e = &s_lib.episodes[s_lib.episode_count++];
    memset(e, 0, sizeof *e);
    copy_episode(&e->episode, episode);
    return e;
}

/* ------------------------------------------------------------------ */
/* The file                                                             */
/* ------------------------------------------------------------------ */

/* Writes an object's members, a comma before each but the first. */
typedef struct {
    FILE *f;
    bool  first;
} member_writer_t;

static void member(member_writer_t *w, const char *key)
{
    fprintf(w->f, "%s\"%s\":", w->first ? "" : ",", key);
    w->first = false;
}

static void write_string(member_writer_t *w, const char *key, const char *value)
{
    member(w, key);
    fputc('"', w->f);
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(w->f, "\\%c", *p);
        else if (*p == '\n') fputs("\\n", w->f);
        else if (*p < 0x20) fprintf(w->f, "\\u%04x", *p);
        else fputc(*p, w->f);
    }
    fputc('"', w->f);
}

static void write_int(member_writer_t *w, const char *key, int64_t value)
{
    member(w, key);
    fprintf(w->f, "%" PRId64, value);
}

static bool write_file(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    /* One buffer for the whole write, rather than stdio's small one. */
    static char *buf;
    if (!buf) buf = malloc(8192);
    if (buf) setvbuf(f, buf, _IOFBF, 8192);

    fputs("{\"version\":1,\"feeds\":[", f);
    for (int i = 0; i < s_lib.feed_count; i++) {
        const pod_lib_feed_t *lf = &s_lib.feeds[i];
        const pod_feed_t *p = &lf->feed;
        member_writer_t w = {.f = f, .first = true};
        fputs(i ? ",\n{" : "\n{", f);
        write_int(&w, "id", p->id);
        write_string(&w, "title", p->title);
        write_string(&w, "author", p->author);
        write_string(&w, "categories", p->categories);
        write_string(&w, "language", p->language);
        write_string(&w, "description", p->description);
        write_int(&w, "episode_count", p->episode_count);
        write_int(&w, "newest", p->newest);
        write_int(&w, "starred_at", lf->starred_at);
        write_int(&w, "viewed_at", lf->viewed_at);
        fputc('}', f);
    }
    fputs("],\"episodes\":[", f);
    for (int i = 0; i < s_lib.episode_count; i++) {
        const pod_lib_episode_t *le = &s_lib.episodes[i];
        const pod_episode_t *e = &le->episode;
        member_writer_t w = {.f = f, .first = true};
        fputs(i ? ",\n{" : "\n{", f);
        write_int(&w, "id", e->id);
        write_int(&w, "feed_id", e->feed_id);
        write_string(&w, "title", e->title);
        write_string(&w, "feed_title", e->feed_title);
        write_string(&w, "url", e->url);
        write_string(&w, "type", e->type);
        write_int(&w, "bytes", e->bytes);
        write_int(&w, "duration", e->duration);
        write_int(&w, "published", e->published);
        write_string(&w, "description", e->description);
        write_int(&w, "starred_at", le->starred_at);
        write_int(&w, "viewed_at", le->viewed_at);
        write_int(&w, "played_at", le->played_at);
        write_int(&w, "saved_at", le->saved_at);
        write_int(&w, "position", le->position);
        write_int(&w, "finished", le->finished);
        fputc('}', f);
    }
    fputs("]}\n", f);

    bool ok = !ferror(f);
    if (fflush(f) != 0) ok = false;
    if (ok) fsync(fileno(f));
    if (fclose(f) != 0) ok = false;
    return ok;
}

static void read_feed_entry(pod_json_t *j, pod_lib_feed_t *lf)
{
    memset(lf, 0, sizeof *lf);
    pod_feed_t *p = &lf->feed;
    char key[24];
    int64_t v;
    if (!pod_json_object(j)) return;
    while (pod_json_key(j, key, sizeof key)) {
        if (strcmp(key, "id") == 0) pod_json_int(j, &p->id);
        else if (strcmp(key, "title") == 0) pod_json_string(j, p->title, sizeof p->title);
        else if (strcmp(key, "author") == 0) pod_json_string(j, p->author, sizeof p->author);
        else if (strcmp(key, "categories") == 0) pod_json_string(j, p->categories, sizeof p->categories);
        else if (strcmp(key, "language") == 0) pod_json_string(j, p->language, sizeof p->language);
        else if (strcmp(key, "description") == 0) pod_json_string(j, p->description, POD_LIB_DESC_MAX);
        else if (strcmp(key, "episode_count") == 0 && pod_json_int(j, &v)) p->episode_count = (int32_t)v;
        else if (strcmp(key, "newest") == 0) pod_json_int(j, &p->newest);
        else if (strcmp(key, "starred_at") == 0) pod_json_int(j, &lf->starred_at);
        else if (strcmp(key, "viewed_at") == 0) pod_json_int(j, &lf->viewed_at);
        else pod_json_skip(j);
    }
}

static void read_episode_entry(pod_json_t *j, pod_lib_episode_t *le)
{
    memset(le, 0, sizeof *le);
    pod_episode_t *e = &le->episode;
    char key[24];
    int64_t v;
    if (!pod_json_object(j)) return;
    while (pod_json_key(j, key, sizeof key)) {
        if (strcmp(key, "id") == 0) pod_json_int(j, &e->id);
        else if (strcmp(key, "feed_id") == 0) pod_json_int(j, &e->feed_id);
        else if (strcmp(key, "title") == 0) pod_json_string(j, e->title, sizeof e->title);
        else if (strcmp(key, "feed_title") == 0) pod_json_string(j, e->feed_title, sizeof e->feed_title);
        else if (strcmp(key, "url") == 0) pod_json_string(j, e->url, sizeof e->url);
        else if (strcmp(key, "type") == 0) pod_json_string(j, e->type, sizeof e->type);
        else if (strcmp(key, "bytes") == 0) pod_json_int(j, &e->bytes);
        else if (strcmp(key, "duration") == 0 && pod_json_int(j, &v)) e->duration = (int32_t)v;
        else if (strcmp(key, "published") == 0) pod_json_int(j, &e->published);
        else if (strcmp(key, "description") == 0) pod_json_string(j, e->description, POD_LIB_DESC_MAX);
        else if (strcmp(key, "starred_at") == 0) pod_json_int(j, &le->starred_at);
        else if (strcmp(key, "viewed_at") == 0) pod_json_int(j, &le->viewed_at);
        else if (strcmp(key, "played_at") == 0) pod_json_int(j, &le->played_at);
        else if (strcmp(key, "saved_at") == 0) pod_json_int(j, &le->saved_at);
        else if (strcmp(key, "position") == 0 && pod_json_int(j, &v)) le->position = (int32_t)v;
        else if (strcmp(key, "finished") == 0 && pod_json_int(j, &v)) le->finished = v != 0;
        else pod_json_skip(j);
    }
}

/* Reads the library from `path` into the arrays: false, with them empty,
 * when it is not there or not whole. */
static bool read_file(const char *path)
{
    s_lib.feed_count = s_lib.episode_count = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *text = size > 0 && size <= FILE_MAX ? malloc((size_t)size) : NULL;
    bool ok = text && fread(text, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) {
        free(text);
        return false;
    }

    pod_json_t j;
    pod_json_init(&j, text, (size_t)size);
    char key[24];
    if (pod_json_object(&j)) {
        while (pod_json_key(&j, key, sizeof key)) {
            if (strcmp(key, "feeds") == 0 && pod_json_array(&j)) {
                while (pod_json_next(&j)) {
                    if (s_lib.feed_count == POD_LIB_FEEDS_MAX) {
                        pod_json_skip(&j);
                        continue;
                    }
                    read_feed_entry(&j, &s_lib.feeds[s_lib.feed_count]);
                    if (s_lib.feeds[s_lib.feed_count].feed.id) s_lib.feed_count++;
                }
            } else if (strcmp(key, "episodes") == 0 && pod_json_array(&j)) {
                while (pod_json_next(&j)) {
                    if (s_lib.episode_count == POD_LIB_EPISODES_MAX) {
                        pod_json_skip(&j);
                        continue;
                    }
                    read_episode_entry(&j, &s_lib.episodes[s_lib.episode_count]);
                    if (s_lib.episodes[s_lib.episode_count].episode.id) s_lib.episode_count++;
                }
            } else {
                pod_json_skip(&j);
            }
        }
    }
    free(text);
    /* A file cut short reads as nothing rather than as half of itself. */
    if (j.error) {
        s_lib.feed_count = s_lib.episode_count = 0;
        return false;
    }
    return true;
}

static void make_dirs(const char *dir)
{
    char path[sizeof s_lib.dir];
    pod_text_copy(path, sizeof path, dir);
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(path, 0755);
        *p = '/';
    }
    mkdir(path, 0755);
}

bool pod_lib_open(const char *dir)
{
    pthread_mutex_lock(&s_lock);
    if (s_lib.open && strcmp(s_lib.dir, dir) == 0) {
        pthread_mutex_unlock(&s_lock);
        return true;
    }
    bool ok = ensure_arrays();
    s_lib.open = ok;
    s_lib.dirty = false;
    pod_text_copy(s_lib.dir, sizeof s_lib.dir, dir);
    if (ok) {
        char path[sizeof s_lib.dir + 32], tmp[sizeof path];
        snprintf(path, sizeof path, "%s/" FILE_NAME, dir);
        snprintf(tmp, sizeof tmp, "%s/" FILE_NAME ".tmp", dir);
        ok = read_file(path);
        /* A save cut short between taking the old file away and renaming
         * the new one leaves only the new one. */
        if (!ok && read_file(tmp)) {
            ok = true;
            rename(tmp, path);
        }
    }
    pthread_mutex_unlock(&s_lock);
    return ok;
}

bool pod_lib_flush(void)
{
    pthread_mutex_lock(&s_lock);
    bool ok = true;
    if (s_lib.open && s_lib.dirty) {
        char path[sizeof s_lib.dir + 32], tmp[sizeof path];
        snprintf(path, sizeof path, "%s/" FILE_NAME, s_lib.dir);
        snprintf(tmp, sizeof tmp, "%s/" FILE_NAME ".tmp", s_lib.dir);
        make_dirs(s_lib.dir);
        prune();
        ok = write_file(tmp);
        /* FAT will not rename over a file. */
        if (ok && remove(path) != 0 && errno != ENOENT) ok = false;
        if (ok && rename(tmp, path) != 0) ok = false;
        if (ok) s_lib.dirty = false;
    }
    pthread_mutex_unlock(&s_lock);
    return ok;
}

void pod_lib_close(void)
{
    pthread_mutex_lock(&s_lock);
    free(s_lib.feeds);
    free(s_lib.episodes);
    memset(&s_lib, 0, sizeof s_lib);
    pthread_mutex_unlock(&s_lock);
}

/* ------------------------------------------------------------------ */
/* Podcasts                                                             */
/* ------------------------------------------------------------------ */

void pod_lib_star_feed(const pod_feed_t *feed, bool starred, int64_t now)
{
    pthread_mutex_lock(&s_lock);
    pod_lib_feed_t *f = feed_entry(feed);
    if (f) {
        f->starred_at = starred ? (f->starred_at ? f->starred_at : now) : 0;
        s_lib.dirty = true;
    }
    pthread_mutex_unlock(&s_lock);
}

void pod_lib_view_feed(const pod_feed_t *feed, int64_t now)
{
    pthread_mutex_lock(&s_lock);
    pod_lib_feed_t *f = feed_entry(feed);
    if (f) {
        f->viewed_at = now;
        s_lib.dirty = true;
    }
    pthread_mutex_unlock(&s_lock);
}

bool pod_lib_feed(int64_t id, pod_lib_feed_t *out)
{
    bool found = false;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_lib.feed_count && !found; i++) {
        if (s_lib.feeds[i].feed.id == id) {
            *out = s_lib.feeds[i];
            found = true;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return found;
}

bool pod_lib_feed_starred(int64_t id)
{
    bool starred = false;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_lib.feed_count; i++) {
        if (s_lib.feeds[i].feed.id == id) starred = s_lib.feeds[i].starred_at != 0;
    }
    pthread_mutex_unlock(&s_lock);
    return starred;
}

static bool feed_in(const pod_lib_feed_t *f, pod_lib_list_t which)
{
    return which == POD_LIB_STARRED ? f->starred_at != 0 : which == POD_LIB_VIEWED && f->viewed_at != 0;
}

static bool episode_in(const pod_lib_episode_t *e, pod_lib_list_t which)
{
    return which == POD_LIB_STARRED ? e->starred_at != 0
         : which == POD_LIB_VIEWED  ? e->viewed_at != 0
         : which == POD_LIB_SAVED   ? e->saved_at != 0
         : e->played_at != 0 && !e->finished && e->position > 0;
}

/* The lists sort indices into the arrays, under the lock, by the key in
 * s_sort_by: no copy of the library is made to sort it. */
static pod_lib_list_t s_sort_by;

static int feed_cmp(const void *a, const void *b)
{
    const pod_lib_feed_t *x = &s_lib.feeds[*(const int *)a], *y = &s_lib.feeds[*(const int *)b];
    if (s_sort_by == POD_LIB_STARRED) return strcasecmp(x->feed.title, y->feed.title);
    return cmp_desc(x->viewed_at, y->viewed_at);
}

int pod_lib_feeds(pod_lib_list_t which, pod_feed_t *out, int max)
{
    int order[POD_LIB_FEEDS_MAX];
    pthread_mutex_lock(&s_lock);
    int n = 0;
    for (int i = 0; i < s_lib.feed_count; i++) {
        if (feed_in(&s_lib.feeds[i], which)) order[n++] = i;
    }
    s_sort_by = which;
    qsort(order, (size_t)n, sizeof order[0], feed_cmp);
    if (which == POD_LIB_VIEWED && n > POD_LIB_RECENT) n = POD_LIB_RECENT;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = s_lib.feeds[order[i]].feed;
    pthread_mutex_unlock(&s_lock);
    return n;
}

int pod_lib_starred_feed_ids(int64_t *out, int max)
{
    pthread_mutex_lock(&s_lock);
    int n = 0;
    for (int i = 0; i < s_lib.feed_count && n < max; i++) {
        if (s_lib.feeds[i].starred_at) out[n++] = s_lib.feeds[i].feed.id;
    }
    pthread_mutex_unlock(&s_lock);
    return n;
}

/* ------------------------------------------------------------------ */
/* Episodes                                                             */
/* ------------------------------------------------------------------ */

void pod_lib_star_episode(const pod_episode_t *episode, bool starred, int64_t now)
{
    pthread_mutex_lock(&s_lock);
    pod_lib_episode_t *e = episode_entry(episode);
    if (e) {
        e->starred_at = starred ? (e->starred_at ? e->starred_at : now) : 0;
        s_lib.dirty = true;
    }
    pthread_mutex_unlock(&s_lock);
}

void pod_lib_view_episode(const pod_episode_t *episode, int64_t now)
{
    pthread_mutex_lock(&s_lock);
    pod_lib_episode_t *e = episode_entry(episode);
    if (e) {
        e->viewed_at = now;
        s_lib.dirty = true;
    }
    pthread_mutex_unlock(&s_lock);
}

void pod_lib_play_episode(const pod_episode_t *episode, int32_t position, bool finished, int64_t now)
{
    pthread_mutex_lock(&s_lock);
    pod_lib_episode_t *e = episode_entry(episode);
    if (e) {
        e->played_at = now;
        e->position = finished ? 0 : position < 0 ? 0 : position;
        e->finished = finished;
        s_lib.dirty = true;
    }
    pthread_mutex_unlock(&s_lock);
}

void pod_lib_save_episode(const pod_episode_t *episode, int64_t saved_at)
{
    pthread_mutex_lock(&s_lock);
    pod_lib_episode_t *e = saved_at ? episode_entry(episode) : find_episode(episode->id);
    if (e) {
        e->saved_at = saved_at;
        s_lib.dirty = true;
    }
    pthread_mutex_unlock(&s_lock);
}

bool pod_lib_episode(int64_t id, pod_lib_episode_t *out)
{
    pthread_mutex_lock(&s_lock);
    const pod_lib_episode_t *e = s_lib.episodes ? find_episode(id) : NULL;
    if (e) {
        *out = *e;
    } else {
        memset(out, 0, sizeof *out);
        out->episode.id = id;
    }
    pthread_mutex_unlock(&s_lock);
    return e != NULL;
}

static int64_t episode_key(const pod_lib_episode_t *e)
{
    switch (s_sort_by) {
    case POD_LIB_STARRED: return e->starred_at;
    case POD_LIB_VIEWED:  return e->viewed_at;
    case POD_LIB_SAVED:   return e->saved_at;
    default:              return e->played_at;
    }
}

static int episode_cmp(const void *a, const void *b)
{
    return cmp_desc(episode_key(&s_lib.episodes[*(const int *)a]),
                    episode_key(&s_lib.episodes[*(const int *)b]));
}

int pod_lib_count_feeds(pod_lib_list_t which)
{
    pthread_mutex_lock(&s_lock);
    int n = 0;
    for (int i = 0; i < s_lib.feed_count; i++) n += feed_in(&s_lib.feeds[i], which);
    pthread_mutex_unlock(&s_lock);
    return which == POD_LIB_VIEWED && n > POD_LIB_RECENT ? POD_LIB_RECENT : n;
}

int pod_lib_count_episodes(pod_lib_list_t which)
{
    pthread_mutex_lock(&s_lock);
    int n = 0;
    for (int i = 0; i < s_lib.episode_count; i++) n += episode_in(&s_lib.episodes[i], which);
    pthread_mutex_unlock(&s_lock);
    return which == POD_LIB_VIEWED && n > POD_LIB_RECENT ? POD_LIB_RECENT : n;
}

int pod_lib_episodes(pod_lib_list_t which, pod_lib_episode_t *out, int max)
{
    int order[POD_LIB_EPISODES_MAX];
    pthread_mutex_lock(&s_lock);
    int n = 0;
    for (int i = 0; i < s_lib.episode_count; i++) {
        if (episode_in(&s_lib.episodes[i], which)) order[n++] = i;
    }
    s_sort_by = which;
    qsort(order, (size_t)n, sizeof order[0], episode_cmp);
    if (which == POD_LIB_VIEWED && n > POD_LIB_RECENT) n = POD_LIB_RECENT;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = s_lib.episodes[order[i]];
    pthread_mutex_unlock(&s_lock);
    return n;
}

static int published_cmp(const void *a, const void *b)
{
    return cmp_desc(s_lib.episodes[*(const int *)a].episode.published,
                    s_lib.episodes[*(const int *)b].episode.published);
}

int pod_lib_feed_episodes(int64_t feed_id, pod_lib_episode_t *out, int max)
{
    int order[POD_LIB_EPISODES_MAX];
    pthread_mutex_lock(&s_lock);
    int n = 0;
    for (int i = 0; i < s_lib.episode_count; i++) {
        if (s_lib.episodes[i].episode.feed_id == feed_id) order[n++] = i;
    }
    qsort(order, (size_t)n, sizeof order[0], published_cmp);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = s_lib.episodes[order[i]];
    pthread_mutex_unlock(&s_lock);
    return n;
}
