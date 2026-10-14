// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// The in-place JSON reader: walking objects and arrays, strings and their
// escapes, numbers too large for 32 bits, skipping, and malformed text.

#include "podcasts/pod_json.h"

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

static void init(pod_json_t *j, const char *text)
{
    pod_json_init(j, text, strlen(text));
}

static void test_walk(void)
{
    pod_json_t j;
    init(&j, " { \"status\" : \"true\", \"feeds\": [ {\"id\": 75075, \"title\": \"Batman\","
             " \"categories\": {\"104\": \"Tv\", \"105\": \"Film\"}, \"dead\": 0},"
             " {\"id\": 61668942832, \"title\": null, \"explicit\": false} ], \"count\": 2 } ");
    char key[32], title[64], cat[32];
    int64_t id = 0, count = 0;
    int feeds = 0, cats = 0;
    EXPECT(pod_json_object(&j));
    while (pod_json_key(&j, key, sizeof key)) {
        if (strcmp(key, "feeds") == 0) {
            EXPECT(pod_json_array(&j));
            while (pod_json_next(&j)) {
                EXPECT(pod_json_object(&j));
                feeds++;
                while (pod_json_key(&j, key, sizeof key)) {
                    if (strcmp(key, "id") == 0) {
                        EXPECT(pod_json_int(&j, &id));
                    } else if (strcmp(key, "title") == 0) {
                        EXPECT(pod_json_string(&j, title, sizeof title));
                        if (feeds == 1) EXPECT(strcmp(title, "Batman") == 0);
                        if (feeds == 2) EXPECT(title[0] == '\0');
                    } else if (strcmp(key, "categories") == 0) {
                        EXPECT(pod_json_object(&j));
                        while (pod_json_key(&j, key, sizeof key)) {
                            EXPECT(pod_json_string(&j, cat, sizeof cat));
                            cats++;
                        }
                    } else {
                        pod_json_skip(&j);
                    }
                }
            }
        } else if (strcmp(key, "count") == 0) {
            EXPECT(pod_json_int(&j, &count));
        } else {
            pod_json_skip(&j);
        }
    }
    EXPECT(!j.error);
    EXPECT(feeds == 2);
    EXPECT(cats == 2);
    EXPECT(id == 61668942832LL);  /* episode ids pass 2^32 */
    EXPECT(count == 2);
}

static void test_escapes(void)
{
    pod_json_t j;
    char out[64];
    init(&j, "\"a\\\"b\\\\c\\/d\\n\\u00e9\\u2019\\ud83c\\udf99\"");
    EXPECT(pod_json_string(&j, out, sizeof out));
    EXPECT(strcmp(out, "a\"b\\c/d\n\xC3\xA9\xE2\x80\x99\xF0\x9F\x8E\x99") == 0);

    /* A lone surrogate, and \u0000, which would end the string, read as U+FFFD. */
    init(&j, "\"\\udc00x\\u0000\"");
    EXPECT(pod_json_string(&j, out, sizeof out));
    EXPECT(strcmp(out, "\xEF\xBF\xBDx\xEF\xBF\xBD") == 0);

    /* Raw UTF-8 passes through. */
    init(&j, "\"caf\xC3\xA9\"");
    EXPECT(pod_json_string(&j, out, sizeof out));
    EXPECT(strcmp(out, "caf\xC3\xA9") == 0);
}

static void test_cut_between_characters(void)
{
    pod_json_t j;
    char out[5];
    /* "abé" is 4 bytes and fits; one more é would not. */
    init(&j, "[\"ab\xC3\xA9\xC3\xA9\", \"next\"]");
    EXPECT(pod_json_array(&j));
    EXPECT(pod_json_next(&j));
    EXPECT(pod_json_string(&j, out, sizeof out));
    EXPECT(strcmp(out, "ab\xC3\xA9") == 0);
    /* Nothing after a character that did not fit, even one that would. */
    init(&j, "\"abc\\u00e9d\"");
    EXPECT(pod_json_string(&j, out, sizeof out));
    EXPECT(strcmp(out, "abc") == 0);
    /* The rest of the string was still passed. */
    init(&j, "[\"0123456789\", \"x\"]");
    EXPECT(pod_json_array(&j));
    EXPECT(pod_json_next(&j));
    EXPECT(pod_json_string(&j, out, sizeof out));
    EXPECT(strcmp(out, "0123") == 0);
    EXPECT(pod_json_next(&j));
    EXPECT(pod_json_string(&j, out, sizeof out));
    EXPECT(strcmp(out, "x") == 0);
    EXPECT(!pod_json_next(&j));
    EXPECT(!j.error);
}

static void test_numbers(void)
{
    pod_json_t j;
    int64_t v;
    char out[16];
    init(&j, "[-12, 3.75, \"1851\", true, false, null, 1e3, \"x\", {}]");
    EXPECT(pod_json_array(&j));
    EXPECT(pod_json_next(&j) && pod_json_int(&j, &v) && v == -12);
    EXPECT(pod_json_next(&j) && pod_json_int(&j, &v) && v == 3);
    EXPECT(pod_json_next(&j) && pod_json_int(&j, &v) && v == 1851);
    EXPECT(pod_json_next(&j) && pod_json_int(&j, &v) && v == 1);
    EXPECT(pod_json_next(&j) && pod_json_int(&j, &v) && v == 0);
    EXPECT(pod_json_next(&j) && pod_json_int(&j, &v) && v == 0);
    EXPECT(pod_json_next(&j) && pod_json_int(&j, &v) && v == 1);  /* the fraction and exponent dropped */
    EXPECT(pod_json_next(&j) && !pod_json_int(&j, &v) && v == 0);
    EXPECT(pod_json_next(&j) && !pod_json_int(&j, &v));
    EXPECT(!pod_json_next(&j));
    EXPECT(!j.error);

    init(&j, "[42, false]");
    EXPECT(pod_json_array(&j));
    EXPECT(pod_json_next(&j) && pod_json_string(&j, out, sizeof out) && strcmp(out, "42") == 0);
    EXPECT(pod_json_next(&j) && pod_json_string(&j, out, sizeof out) && strcmp(out, "false") == 0);
}

static void test_skip(void)
{
    pod_json_t j;
    char key[16];
    int64_t v = 0;
    init(&j, "{\"a\": {\"b\": [1, {\"c\": \"]}\\\"\"}], \"d\": []}, \"e\": 5}");
    EXPECT(pod_json_object(&j));
    EXPECT(pod_json_key(&j, key, sizeof key) && strcmp(key, "a") == 0);
    pod_json_skip(&j);
    EXPECT(pod_json_key(&j, key, sizeof key) && strcmp(key, "e") == 0);
    EXPECT(pod_json_int(&j, &v) && v == 5);
    EXPECT(!pod_json_key(&j, key, sizeof key));
    EXPECT(!j.error);

    /* Asking for an object where there is none passes the value. */
    init(&j, "[\"s\", 7]");
    EXPECT(pod_json_array(&j));
    EXPECT(pod_json_next(&j));
    EXPECT(!pod_json_object(&j));
    EXPECT(pod_json_next(&j) && pod_json_int(&j, &v) && v == 7);
}

static void test_malformed(void)
{
    const char *bad[] = {
        "", "{", "{\"a\"", "{\"a\": ", "[1, 2", "\"open", "{\"a\" 1}", "{a: 1}", "[1,]",
        "\"bad \\x escape\"", "\"\\u12\"",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pod_json_t j;
        char key[8], s[8];
        int64_t v;
        int guard = 0;
        init(&j, bad[i]);
        /* Whatever is asked of it, it ends, with the error set. */
        if (pod_json_object(&j)) {
            while (pod_json_key(&j, key, sizeof key) && guard++ < 10) pod_json_skip(&j);
        } else if (pod_json_array(&j)) {
            while (pod_json_next(&j) && guard++ < 10) pod_json_int(&j, &v);
        } else {
            pod_json_string(&j, s, sizeof s);
        }
        if (!j.error && strcmp(bad[i], "") != 0) {
            fprintf(stderr, "%s:%d: no error for %s\n", __FILE__, __LINE__, bad[i]);
            s_failures++;
        }
        EXPECT(guard < 10);
    }
}

int main(void)
{
    test_walk();
    test_escapes();
    test_cut_between_characters();
    test_numbers();
    test_skip();
    test_malformed();
    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_pod_json: all checks passed\n");
    return 0;
}
