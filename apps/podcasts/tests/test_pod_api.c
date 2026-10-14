// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// PodcastIndex requests and answers. The answers are shaped like the API's
// own, field for field, down to the nulls and the ids past 2^32.

#include "podcasts/pod_api.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            s_failures++;                                                \
        }                                                                \
    } while (0)

#define EXPECT_STR(actual, expected)                                                        \
    do {                                                                                    \
        if (strcmp((actual), (expected)) != 0) {                                            \
            fprintf(stderr, "%s:%d: %s\n  expected: \"%s\"\n  actual:   \"%s\"\n", __FILE__, \
                    __LINE__, #actual, (expected), (actual));                               \
            s_failures++;                                                                   \
        }                                                                                   \
    } while (0)

static void test_paths(void)
{
    char out[256];
    EXPECT(pod_api_search_path(out, sizeof out, POD_SEARCH_TERM, "no agenda", false, 25));
    EXPECT_STR(out, "/search/byterm?q=no%20agenda&max=25");
    EXPECT(pod_api_search_path(out, sizeof out, POD_SEARCH_TERM, "news", true, 10));
    EXPECT_STR(out, "/search/byterm?q=news&max=10&clean");
    EXPECT(pod_api_search_path(out, sizeof out, POD_SEARCH_TITLE, "Batman University", true, 10));
    EXPECT_STR(out, "/search/bytitle?q=Batman%20University&max=10&clean&similar");
    EXPECT(pod_api_search_path(out, sizeof out, POD_SEARCH_PERSON, "Adam Curry", true, 10));
    EXPECT_STR(out, "/search/byperson?q=Adam%20Curry&max=10");
    EXPECT(pod_api_search_path(out, sizeof out, POD_SEARCH_MUSIC, "jazz", false, 5));
    EXPECT_STR(out, "/search/music/byterm?q=jazz&max=5");

    EXPECT(pod_api_trending_path(out, sizeof out, "", "", 40));
    EXPECT_STR(out, "/podcasts/trending?max=40");
    EXPECT(pod_api_trending_path(out, sizeof out, "True Crime", "en", 40));
    EXPECT_STR(out, "/podcasts/trending?max=40&cat=True%20Crime&lang=en");

    EXPECT(pod_api_categories_path(out, sizeof out));
    EXPECT_STR(out, "/categories/list");
    EXPECT(pod_api_feed_path(out, sizeof out, 920666));
    EXPECT_STR(out, "/podcasts/byfeedid?id=920666");
    EXPECT(pod_api_episode_path(out, sizeof out, 61668942832LL));
    EXPECT_STR(out, "/episodes/byid?id=61668942832");

    int64_t ids[] = {75075, 920666, 4884264};
    EXPECT(pod_api_episodes_path(out, sizeof out, ids, 3, 40));
    EXPECT_STR(out, "/episodes/byfeedid?id=75075,920666,4884264&max=40");
    EXPECT(!pod_api_episodes_path(out, sizeof out, ids, 0, 40));

    /* Too long for the buffer: refused, not cut. */
    char small[24];
    EXPECT(!pod_api_search_path(small, sizeof small, POD_SEARCH_TERM, "a long query", false, 25));
    EXPECT(!pod_api_episodes_path(small, sizeof small, ids, 3, 40));
}

static void test_auth(void)
{
    char date[24], auth[41];
    pod_api_auth("UXKCGDSYGUUEVQJSYDZH", "yzJe2eE7XV-3eY576dyRZ6wXyAbndh6LUrCZ8KN|", 1613713388,
                 date, auth);
    EXPECT_STR(date, "1613713388");
    EXPECT_STR(auth, "73a1fffed61c1d30d858beb1fc48f355386449d2");
}

static const char SEARCH[] =
    "{\"status\":\"true\",\"feeds\":[{\"id\":4884264,\"podcastGuid\":\"d97adb6a\","
    "\"title\":\"ALUMA en las Ondas\",\"url\":\"https://www.spreaker.com/show/5228950/episodes/feed\","
    "\"originalUrl\":\"https://www.spreaker.com/show/5228950/episodes/feed\",\"link\":\"https://x\","
    "\"description\":\"<p>En el curso 2020 &ndash; 2021 ALUMA, la asociaci\\u00f3n de alumnos</p>"
    "<p>Segundo p\xC3\xA1rrafo</p>\",\"author\":\"Radiolab UGR\",\"ownerName\":\"Radiolab UGR\","
    "\"image\":\"https://img\",\"artwork\":\"https://img\",\"lastUpdateTime\":1791888094,"
    "\"itunesId\":null,\"generator\":\"\",\"language\":\"es\",\"type\":0,\"dead\":0,"
    "\"categories\":{\"55\":\"News\",\"59\":\"Politics\"},\"locked\":0,\"explicit\":false,"
    "\"medium\":\"podcast\",\"episodeCount\":76,\"imageUrlHash\":2313778665,"
    "\"newestItemPubdate\":1791887402,\"duplicateOf\":null},"
    "{\"id\":75075,\"title\":\"Batman University\",\"author\":\"\",\"ownerName\":\"Tony Sindelar\","
    "\"description\":\"Batman University is a seasonal podcast\",\"categories\":null,"
    "\"episodeCount\":19}],"
    "\"count\":2,\"query\":\"radiolab\",\"description\":\"Found matching feeds\"}";

static void test_feeds(void)
{
    pod_feed_t feeds[4];
    char error[64];
    int n = pod_api_parse_feeds(SEARCH, sizeof SEARCH - 1, feeds, 4, error, sizeof error);
    EXPECT(n == 2);
    EXPECT(feeds[0].id == 4884264);
    EXPECT_STR(feeds[0].title, "ALUMA en las Ondas");
    EXPECT_STR(feeds[0].author, "Radiolab UGR");
    EXPECT_STR(feeds[0].categories, "News, Politics");
    EXPECT_STR(feeds[0].language, "es");
    EXPECT_STR(feeds[0].description,
               "En el curso 2020 \xE2\x80\x93 2021 ALUMA, la asociaci\xC3\xB3n de alumnos\n\n"
               "Segundo p\xC3\xA1rrafo");
    EXPECT(feeds[0].episode_count == 76);
    EXPECT(feeds[0].newest == 1791887402);
    /* No author: the owner's name. */
    EXPECT_STR(feeds[1].author, "Tony Sindelar");
    EXPECT_STR(feeds[1].categories, "");
    EXPECT(feeds[1].newest == 0);

    /* Only as many as asked for. */
    EXPECT(pod_api_parse_feeds(SEARCH, sizeof SEARCH - 1, feeds, 1, error, sizeof error) == 1);

    /* Trending's feeds say when their newest episode came out another way. */
    static const char TRENDING[] =
        "{\"status\":\"true\",\"feeds\":[{\"id\":312559,\"url\":\"https://f\",\"title\":\"Illegal "
        "Curve Hockey\",\"description\":\"d\",\"author\":\"The Sick Podcast\",\"image\":\"i\","
        "\"artwork\":\"a\",\"newestItemPublishTime\":1791980000,\"itunesId\":1,\"trendScore\":9,"
        "\"language\":\"en\",\"categories\":{\"16\":\"Sports\"}}],\"count\":1,\"max\":\"40\","
        "\"since\":null,\"description\":\"Found matching items.\"}";
    EXPECT(pod_api_parse_feeds(TRENDING, sizeof TRENDING - 1, feeds, 4, error, sizeof error) == 1);
    EXPECT(feeds[0].newest == 1791980000);
    EXPECT_STR(feeds[0].categories, "Sports");

    /* /podcasts/byfeedid answers with one feed, or an empty list for none. */
    static const char ONE[] =
        "{\"status\":\"true\",\"query\":{\"id\":\"75075\"},\"feed\":{\"id\":75075,"
        "\"title\":\"Batman University\",\"value\":{\"model\":{}},\"funding\":{\"url\":\"u\"}},"
        "\"description\":\"Found matching feed\"}";
    EXPECT(pod_api_parse_feeds(ONE, sizeof ONE - 1, feeds, 4, error, sizeof error) == 1);
    EXPECT(feeds[0].id == 75075);
    static const char NONE[] =
        "{\"status\":\"true\",\"query\":{\"id\":\"1\"},\"feed\":[],\"description\":\"No feeds match\"}";
    EXPECT(pod_api_parse_feeds(NONE, sizeof NONE - 1, feeds, 4, error, sizeof error) == 0);
}

static const char EPISODES[] =
    "{\"status\":\"true\",\"liveItems\":[],\"items\":[{\"id\":61668942832,"
    "\"title\":\"ALUMA en las Ondas 75: Entrevista\",\"link\":\"https://l\","
    "\"description\":\"En este episodio <b>conversamos</b>\",\"guid\":\"g\","
    "\"datePublished\":1791887402,\"datePublishedPretty\":\"October 13, 2026 5:30am\","
    "\"dateCrawled\":1791888095,"
    "\"enclosureUrl\":\"https://api.spreaker.com/download/episode/75748320/aluma.mp3\","
    "\"enclosureType\":\"audio/mpeg\",\"enclosureLength\":29624511,\"duration\":1851,"
    "\"explicit\":0,\"episode\":null,\"episodeType\":\"full\",\"season\":0,\"image\":\"i\","
    "\"feedItunesId\":null,\"feedUrl\":\"https://f\",\"feedImage\":\"i\",\"feedId\":4884264,"
    "\"podcastGuid\":\"d97\",\"feedLanguage\":\"es\",\"feedDead\":0,\"feedDuplicateOf\":null,"
    "\"chaptersUrl\":null,\"transcriptUrl\":null,\"soundbites\":[{\"startTime\":1,\"duration\":2}],"
    "\"persons\":[{\"id\":1,\"name\":\"n\"}]},"
    "{\"id\":2,\"title\":\"Q&amp;A\",\"feedTitle\":\"Recent Feed\",\"feedId\":9,"
    "\"enclosureUrl\":\"https://x/2.m4a\",\"enclosureType\":\"audio/x-m4a\","
    "\"enclosureLength\":\"1200\",\"duration\":null,\"datePublished\":1}],"
    "\"count\":2,\"query\":\"4884264\",\"description\":\"Found matching items.\"}";

static void test_episodes(void)
{
    pod_episode_t eps[4];
    char error[64];
    int n = pod_api_parse_episodes(EPISODES, sizeof EPISODES - 1, eps, 4, error, sizeof error);
    EXPECT(n == 2);
    EXPECT(eps[0].id == 61668942832LL);
    EXPECT(eps[0].feed_id == 4884264);
    EXPECT_STR(eps[0].title, "ALUMA en las Ondas 75: Entrevista");
    EXPECT_STR(eps[0].feed_title, "");
    EXPECT_STR(eps[0].url, "https://api.spreaker.com/download/episode/75748320/aluma.mp3");
    EXPECT_STR(eps[0].type, "audio/mpeg");
    EXPECT(eps[0].bytes == 29624511);
    EXPECT(eps[0].duration == 1851);
    EXPECT(eps[0].published == 1791887402);
    EXPECT_STR(eps[0].description, "En este episodio conversamos");
    EXPECT(pod_episode_playable(&eps[0]));

    EXPECT_STR(eps[1].title, "Q&A");
    EXPECT_STR(eps[1].feed_title, "Recent Feed");
    EXPECT(eps[1].bytes == 1200);
    EXPECT(eps[1].duration == 0);
    EXPECT(!pod_episode_playable(&eps[1]));

    /* /episodes/byid's one "episode". */
    static const char ONE[] =
        "{\"status\":\"true\",\"id\":\"7\",\"episode\":{\"id\":7,\"title\":\"t\",\"feedId\":3,"
        "\"enclosureUrl\":\"https://e/7.mp3?x=1\",\"enclosureType\":\"\"},\"description\":\"Found\"}";
    EXPECT(pod_api_parse_episodes(ONE, sizeof ONE - 1, eps, 4, error, sizeof error) == 1);
    EXPECT(eps[0].id == 7);
    /* No type: the extension says MP3. */
    EXPECT(pod_episode_playable(&eps[0]));
}

static void test_playable(void)
{
    pod_episode_t e;
    memset(&e, 0, sizeof e);
    strcpy(e.url, "https://dts.podtrac.com/redirect.mp3/x/ep.mp3?source=rss");
    strcpy(e.type, "audio/mpeg");
    EXPECT(pod_episode_playable(&e));
    strcpy(e.type, "AUDIO/MP3");
    EXPECT(pod_episode_playable(&e));
    strcpy(e.type, "application/octet-stream");
    EXPECT(pod_episode_playable(&e));
    strcpy(e.type, "video/mp4");
    EXPECT(!pod_episode_playable(&e));
    strcpy(e.type, "");
    strcpy(e.url, "https://x/ep.m4a");
    EXPECT(!pod_episode_playable(&e));
    e.url[0] = '\0';
    strcpy(e.type, "audio/mpeg");
    EXPECT(!pod_episode_playable(&e));
}

static void test_categories(void)
{
    static const char CATS[] =
        "{\"status\":\"true\",\"feeds\":[{\"id\":1,\"name\":\"Arts\"},{\"id\":2,\"name\":\"Books\"},"
        "{\"id\":0,\"name\":\"\"}],\"count\":3,\"description\":\"Categories\"}";
    pod_category_t cats[8];
    char error[64];
    EXPECT(pod_api_parse_categories(CATS, sizeof CATS - 1, cats, 8, error, sizeof error) == 2);
    EXPECT(cats[0].id == 1);
    EXPECT_STR(cats[1].name, "Books");
}

static void test_failures(void)
{
    pod_feed_t feeds[2];
    char error[64];
    static const char REFUSED[] =
        "{\"status\":\"false\",\"description\":\"Authorization header value is invalid.\"}";
    EXPECT(pod_api_parse_feeds(REFUSED, sizeof REFUSED - 1, feeds, 2, error, sizeof error) == -1);
    EXPECT_STR(error, "Authorization header value is invalid.");

    static const char HTML[] = "<html><body>502 Bad Gateway</body></html>";
    EXPECT(pod_api_parse_feeds(HTML, sizeof HTML - 1, feeds, 2, error, sizeof error) == -1);
    EXPECT_STR(error, "Unexpected answer");

    static const char CUT[] = "{\"status\":\"true\",\"feeds\":[{\"id\":1,\"title\":\"cut sh";
    EXPECT(pod_api_parse_feeds(CUT, sizeof CUT - 1, feeds, 2, error, sizeof error) == -1);

    /* An answer with no list at all is not an empty one. */
    static const char NO_LIST[] = "{\"status\":\"true\",\"count\":0}";
    EXPECT(pod_api_parse_feeds(NO_LIST, sizeof NO_LIST - 1, feeds, 2, error, sizeof error) == -1);
    static const char EMPTY[] = "{\"status\":\"true\",\"feeds\":[],\"count\":0}";
    EXPECT(pod_api_parse_feeds(EMPTY, sizeof EMPTY - 1, feeds, 2, error, sizeof error) == 0);
}

int main(void)
{
    test_paths();
    test_auth();
    test_feeds();
    test_episodes();
    test_playable();
    test_categories();
    test_failures();
    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_pod_api: all checks passed\n");
    return 0;
}
