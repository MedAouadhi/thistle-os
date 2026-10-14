// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// The library: its lists and their order, what it keeps and drops, and its
// file, written and read back, including after a save cut short.

#include "podcasts/pod_library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int s_failures;
static char s_dir[256];

#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            s_failures++;                                                \
        }                                                                \
    } while (0)

static pod_feed_t feed(int64_t id, const char *title)
{
    pod_feed_t f;
    memset(&f, 0, sizeof f);
    f.id = id;
    snprintf(f.title, sizeof f.title, "%s", title);
    snprintf(f.author, sizeof f.author, "Author %d", (int)id);
    return f;
}

static pod_episode_t episode(int64_t id, int64_t feed_id)
{
    pod_episode_t e;
    memset(&e, 0, sizeof e);
    e.id = id;
    e.feed_id = feed_id;
    snprintf(e.title, sizeof e.title, "Episode %lld", (long long)id);
    snprintf(e.url, sizeof e.url, "https://example.com/%lld.mp3", (long long)id);
    snprintf(e.type, sizeof e.type, "audio/mpeg");
    e.duration = 1800;
    return e;
}

static void reset(const char *name)
{
    pod_lib_close();
    snprintf(s_dir, sizeof s_dir, "%s/%s", getenv("POD_TEST_DIR") ? getenv("POD_TEST_DIR") : "/tmp", name);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", s_dir);
    if (system(cmd) != 0) s_failures++;
    EXPECT(!pod_lib_open(s_dir));  /* nothing there yet */
}

static void test_feeds(void)
{
    reset("feeds");
    pod_feed_t b = feed(2, "beta"), a = feed(1, "Alpha"), c = feed(3, "Gamma");
    pod_lib_star_feed(&b, true, 100);
    pod_lib_star_feed(&a, true, 200);
    pod_lib_view_feed(&c, 300);
    pod_lib_view_feed(&a, 400);

    pod_feed_t out[8];
    /* Starred by title, whatever the case. */
    EXPECT(pod_lib_feeds(POD_LIB_STARRED, out, 8) == 2);
    EXPECT(out[0].id == 1 && out[1].id == 2);
    /* Viewed last first. */
    EXPECT(pod_lib_feeds(POD_LIB_VIEWED, out, 8) == 2);
    EXPECT(out[0].id == 1 && out[1].id == 3);
    EXPECT(pod_lib_feed_starred(1) && !pod_lib_feed_starred(3) && !pod_lib_feed_starred(99));
    EXPECT(pod_lib_count_feeds(POD_LIB_STARRED) == 2 && pod_lib_count_feeds(POD_LIB_VIEWED) == 2);

    int64_t ids[8];
    EXPECT(pod_lib_starred_feed_ids(ids, 8) == 2);

    /* Unstarred and never viewed: gone once the library is written. */
    pod_lib_star_feed(&b, false, 500);
    EXPECT(pod_lib_feeds(POD_LIB_STARRED, out, 8) == 1);
    EXPECT(pod_lib_flush());
    pod_lib_feed_t lf;
    EXPECT(!pod_lib_feed(2, &lf));

    /* Starring again keeps when it was first starred. */
    pod_lib_star_feed(&a, true, 900);
    EXPECT(pod_lib_feed(1, &lf) && lf.starred_at == 200);

    /* Only so many viewed are kept. */
    for (int i = 0; i < POD_LIB_RECENT + 5; i++) {
        pod_feed_t f = feed(100 + i, "many");
        pod_lib_view_feed(&f, 1000 + i);
    }
    EXPECT(pod_lib_feeds(POD_LIB_VIEWED, out, 8) == 8);
    EXPECT(out[0].id == 100 + POD_LIB_RECENT + 4);
    pod_feed_t all[POD_LIB_FEEDS_MAX];
    EXPECT(pod_lib_feeds(POD_LIB_VIEWED, all, POD_LIB_FEEDS_MAX) == POD_LIB_RECENT);
    EXPECT(pod_lib_flush());
    /* The starred one stayed, though viewed long ago. */
    EXPECT(pod_lib_feed_starred(1));
}

static void test_episodes(void)
{
    reset("episodes");
    pod_episode_t e1 = episode(61668942832LL, 7), e2 = episode(2, 7), e3 = episode(3, 8);
    pod_lib_star_episode(&e1, true, 10);
    pod_lib_star_episode(&e3, true, 30);
    pod_lib_view_episode(&e2, 40);
    pod_lib_play_episode(&e2, 125, false, 50);
    pod_lib_play_episode(&e3, 0, true, 60);
    pod_lib_save_episode(&e1, 70);

    pod_lib_episode_t out[8];
    EXPECT(pod_lib_episodes(POD_LIB_STARRED, out, 8) == 2);
    EXPECT(out[0].episode.id == 3 && out[1].episode.id == 61668942832LL);
    EXPECT(pod_lib_episodes(POD_LIB_SAVED, out, 8) == 1);
    EXPECT(out[0].episode.id == 61668942832LL && out[0].saved_at == 70);
    /* Begun and not finished: e3 was played to the end. */
    EXPECT(pod_lib_episodes(POD_LIB_CONTINUE, out, 8) == 1);
    EXPECT(out[0].episode.id == 2 && out[0].position == 125);
    EXPECT(pod_lib_episodes(POD_LIB_VIEWED, out, 8) == 1);
    EXPECT(pod_lib_count_episodes(POD_LIB_STARRED) == 2 && pod_lib_count_episodes(POD_LIB_SAVED) == 1);
    EXPECT(pod_lib_count_episodes(POD_LIB_CONTINUE) == 1 && pod_lib_count_episodes(POD_LIB_VIEWED) == 1);

    pod_lib_episode_t one;
    EXPECT(pod_lib_episode(3, &one) && one.finished && one.position == 0);
    EXPECT(!pod_lib_episode(404, &one) && one.episode.id == 404 && !one.starred_at);

    /* One podcast's, newest first. */
    pod_episode_t older = episode(4, 7), newer = episode(5, 7);
    older.published = 100;
    newer.published = 200;
    pod_lib_star_episode(&older, true, 75);
    pod_lib_star_episode(&newer, true, 76);
    EXPECT(pod_lib_feed_episodes(7, out, 8) == 4);
    EXPECT(out[0].episode.id == 5 && out[1].episode.id == 4);
    EXPECT(pod_lib_feed_episodes(8, out, 8) == 1);
    EXPECT(pod_lib_feed_episodes(404, out, 8) == 0);
    pod_lib_star_episode(&older, false, 77);
    pod_lib_star_episode(&newer, false, 77);

    /* A list cut to fit takes the first of the sorted list. */
    EXPECT(pod_lib_episodes(POD_LIB_STARRED, out, 1) == 1 && out[0].episode.id == 3);

    /* An episode met again without its podcast's title keeps the one known. */
    pod_episode_t named = episode(9, 7);
    snprintf(named.feed_title, sizeof named.feed_title, "The Show");
    pod_lib_star_episode(&named, true, 80);
    pod_episode_t unnamed = episode(9, 7);
    pod_lib_view_episode(&unnamed, 90);
    EXPECT(pod_lib_episode(9, &one) && strcmp(one.episode.feed_title, "The Show") == 0);

    /* No longer saved: kept, since it is starred, but not in Saved. */
    pod_lib_save_episode(&e1, 0);
    EXPECT(pod_lib_episodes(POD_LIB_SAVED, out, 8) == 0);
    EXPECT(pod_lib_episode(e1.id, &one));
    /* Un-saving one never known does not add it. */
    pod_episode_t stranger = episode(77, 1);
    pod_lib_save_episode(&stranger, 0);
    EXPECT(!pod_lib_episode(77, &one));

    /* History is cut to its length; what was played last stays. */
    for (int i = 0; i < POD_LIB_HISTORY + 10; i++) {
        pod_episode_t e = episode(1000 + i, 5);
        pod_lib_play_episode(&e, 60, false, 2000 + i);
    }
    EXPECT(pod_lib_flush());
    EXPECT(pod_lib_episode(1000 + POD_LIB_HISTORY + 9, &one));
    EXPECT(!pod_lib_episode(1000, &one));
}

static void test_full(void)
{
    reset("full");
    /* Every slot starred: a new one pushes out the one starred longest ago. */
    for (int i = 0; i < POD_LIB_EPISODES_MAX; i++) {
        pod_episode_t e = episode(i + 1, 1);
        pod_lib_star_episode(&e, true, 100 + i);
    }
    /* ...but never a saved one. */
    pod_episode_t first = episode(1, 1);
    pod_lib_save_episode(&first, 5);
    pod_episode_t extra = episode(5000, 1);
    pod_lib_star_episode(&extra, true, 9999);
    pod_lib_episode_t one;
    EXPECT(pod_lib_episode(5000, &one));
    EXPECT(pod_lib_episode(1, &one));
    EXPECT(!pod_lib_episode(2, &one));
}

static void test_file(void)
{
    reset("file");
    pod_feed_t f = feed(42, "Quotes \"and\" \\slashes\\ and\nlines \xC3\xA9");
    snprintf(f.description, sizeof f.description, "%s", "A description");
    f.episode_count = 12;
    f.newest = 1791887402;
    pod_lib_star_feed(&f, true, 1);
    pod_episode_t e = episode(61668942832LL, 42);
    snprintf(e.feed_title, sizeof e.feed_title, "The \"Show\"");
    e.bytes = 29624511;
    e.published = 1791887402;
    pod_lib_play_episode(&e, 321, false, 2);
    pod_lib_save_episode(&e, 3);
    EXPECT(pod_lib_flush());

    /* A long description is kept short. */
    pod_feed_t g = feed(43, "Long");
    memset(g.description, 'x', sizeof g.description - 1);
    g.description[sizeof g.description - 1] = '\0';
    pod_lib_view_feed(&g, 4);
    EXPECT(pod_lib_flush());

    pod_lib_close();
    EXPECT(pod_lib_open(s_dir));
    pod_lib_feed_t lf;
    EXPECT(pod_lib_feed(42, &lf));
    EXPECT(strcmp(lf.feed.title, f.title) == 0);
    EXPECT(strcmp(lf.feed.description, "A description") == 0);
    EXPECT(lf.feed.episode_count == 12 && lf.feed.newest == 1791887402 && lf.starred_at == 1);
    EXPECT(pod_lib_feed(43, &lf) && strlen(lf.feed.description) == POD_LIB_DESC_MAX - 1);

    pod_lib_episode_t le;
    EXPECT(pod_lib_episode(61668942832LL, &le));
    EXPECT(strcmp(le.episode.feed_title, "The \"Show\"") == 0);
    EXPECT(strcmp(le.episode.url, e.url) == 0);
    EXPECT(le.episode.bytes == 29624511 && le.episode.duration == 1800);
    EXPECT(le.position == 321 && le.played_at == 2 && le.saved_at == 3 && !le.finished);

    /* Nothing changed: nothing is written. */
    char path[300];
    snprintf(path, sizeof path, "%s/library.json", s_dir);
    EXPECT(unlink(path) == 0);
    EXPECT(pod_lib_flush());
    EXPECT(access(path, F_OK) != 0);

    /* A save cut short after the old file went: the new one is taken. */
    pod_lib_star_feed(&f, false, 5);
    EXPECT(pod_lib_flush());
    char tmp[310];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    EXPECT(rename(path, tmp) == 0);
    pod_lib_close();
    EXPECT(pod_lib_open(s_dir));
    EXPECT(pod_lib_episode(61668942832LL, &le));
    EXPECT(!pod_lib_feed_starred(42));
    EXPECT(access(path, F_OK) == 0 && access(tmp, F_OK) != 0);

    /* A file cut short reads as an empty library, not half of one. */
    FILE *file = fopen(path, "r+b");
    EXPECT(file != NULL);
    if (file) {
        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        fclose(file);
        EXPECT(truncate(path, size / 2) == 0);
    }
    pod_lib_close();
    EXPECT(!pod_lib_open(s_dir));
    EXPECT(!pod_lib_episode(61668942832LL, &le));
}

int main(void)
{
    test_feeds();
    test_episodes();
    test_full();
    test_file();
    pod_lib_close();
    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_pod_library: all checks passed\n");
    return 0;
}
