// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// The password, the sessions it opens, and the lock on guessing it.

#include "filexfer/xfer_auth.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            s_failures++;                                                      \
        }                                                                      \
    } while (0)

static void expect_str(const char *actual, const char *expected, int line)
{
    if (strcmp(actual, expected) == 0) return;
    fprintf(stderr, "%s:%d:\n  expected: \"%s\"\n  actual:   \"%s\"\n", __FILE__, line, expected,
            actual);
    s_failures++;
}

#define EXPECT_STR(a, e) expect_str((a), (e), __LINE__)

static const uint8_t PW_RANDOM[XFER_PASSWORD_CHARS] = {0, 1, 8, 9, 31, 32, 255, 100};

static void fresh(xfer_auth_t *a)
{
    xfer_auth_reset(a, PW_RANDOM);
}

static void test_password(void)
{
    xfer_auth_t a;
    fresh(&a);
    /* 0->'2', 1->'3', 8->'A', 9->'B', 31->'Z', 32->'2', 255->'Z', 100->'6' */
    EXPECT_STR(a.password, "23AB-Z2Z6");

    uint8_t all[XFER_PASSWORD_CHARS];
    for (int b = 0; b < 256; b += XFER_PASSWORD_CHARS) {
        for (int i = 0; i < XFER_PASSWORD_CHARS; i++) all[i] = (uint8_t)(b + i);
        xfer_auth_reset(&a, all);
        for (const char *p = a.password; *p; p++) {
            CHECK(*p == '-' || (*p >= '2' && *p <= '9') || (*p >= 'A' && *p <= 'Z'));
            CHECK(*p != 'O' && *p != 'I');
        }
    }
}

static void test_check(void)
{
    xfer_auth_t a;
    fresh(&a);
    CHECK(xfer_auth_check(&a, "23AB-Z2Z6", 0, NULL) == XFER_LOGIN_OK);
    CHECK(xfer_auth_check(&a, "23abz2z6", 0, NULL) == XFER_LOGIN_OK);
    CHECK(xfer_auth_check(&a, "  23ab-z2z6\n", 0, NULL) == XFER_LOGIN_OK);
    CHECK(xfer_auth_check(&a, "23 AB Z2 Z6", 0, NULL) == XFER_LOGIN_OK);
    CHECK(a.failures_total == 0);

    CHECK(xfer_auth_check(&a, "23AB-Z2Z7", 0, NULL) == XFER_LOGIN_WRONG);
    CHECK(xfer_auth_check(&a, "23AB-Z2Z", 0, NULL) == XFER_LOGIN_WRONG);
    CHECK(xfer_auth_check(&a, "23AB-Z2Z66", 0, NULL) == XFER_LOGIN_WRONG);
    CHECK(xfer_auth_check(&a, "", 0, NULL) == XFER_LOGIN_WRONG);
    CHECK(xfer_auth_check(&a, NULL, 0, NULL) == XFER_LOGIN_WRONG);
    CHECK(a.failures_total == 5);

    /* No password at all lets nobody in. */
    xfer_auth_t none;
    memset(&none, 0, sizeof none);
    CHECK(xfer_auth_check(&none, "", 0, NULL) == XFER_LOGIN_WRONG);
}

static void test_lock(void)
{
    xfer_auth_t a;
    fresh(&a);
    uint32_t retry = 99;
    for (int i = 0; i < XFER_FREE_TRIES - 1; i++) {
        CHECK(xfer_auth_check(&a, "WRONGPWD", 1000, &retry) == XFER_LOGIN_WRONG);
        CHECK(retry == 0);
    }
    CHECK(xfer_auth_check(&a, "WRONGPWD", 1000, &retry) == XFER_LOGIN_WRONG);
    CHECK(retry == XFER_LOCK_FIRST_S);
    CHECK(xfer_auth_locked(&a, 1000));

    /* Locked: even the right one is not looked at, and nothing is counted. */
    uint32_t total = a.failures_total;
    CHECK(xfer_auth_check(&a, "23AB-Z2Z6", 1000 + 10000, &retry) == XFER_LOGIN_LOCKED);
    CHECK(retry == XFER_LOCK_FIRST_S - 10);
    CHECK(a.failures_total == total);

    /* Each wrong one after doubles the wait. */
    int64_t t = 1000 + XFER_LOCK_FIRST_S * 1000;
    CHECK(!xfer_auth_locked(&a, t));
    CHECK(xfer_auth_check(&a, "WRONGPWD", t, &retry) == XFER_LOGIN_WRONG);
    CHECK(retry == XFER_LOCK_FIRST_S * 2);
    t += (int64_t)retry * 1000;
    CHECK(xfer_auth_check(&a, "WRONGPWD", t, &retry) == XFER_LOGIN_WRONG);
    CHECK(retry == XFER_LOCK_FIRST_S * 4);
    for (int i = 0; i < 10; i++) {
        t += (int64_t)retry * 1000;
        CHECK(xfer_auth_check(&a, "WRONGPWD", t, &retry) == XFER_LOGIN_WRONG);
    }
    CHECK(retry == XFER_LOCK_MAX_S);

    /* The right one, once let in, ends it. */
    t += (int64_t)retry * 1000;
    CHECK(xfer_auth_check(&a, "23AB-Z2Z6", t, &retry) == XFER_LOGIN_OK);
    CHECK(a.failures == 0);
    CHECK(xfer_auth_check(&a, "WRONGPWD", t, &retry) == XFER_LOGIN_WRONG);
    CHECK(retry == 0);

    /* So does a new password. */
    for (int i = 0; i < XFER_FREE_TRIES; i++) xfer_auth_check(&a, "WRONGPWD", t, NULL);
    CHECK(xfer_auth_locked(&a, t));
    fresh(&a);
    CHECK(!xfer_auth_locked(&a, t));
    CHECK(a.failures_total == 0);
}

static void test_sessions(void)
{
    xfer_auth_t a;
    fresh(&a);
    uint8_t r[XFER_TOKEN_BYTES];
    char tokens[XFER_SESSIONS + 1][XFER_TOKEN_LEN + 1];

    for (int i = 0; i <= XFER_SESSIONS; i++) {
        memset(r, i + 1, sizeof r);
        xfer_auth_open_session(&a, r, 100 + i, tokens[i]);
        CHECK(strlen(tokens[i]) == XFER_TOKEN_LEN);
    }
    EXPECT_STR(tokens[0], "01010101010101010101010101010101");
    CHECK(xfer_auth_session_count(&a) == XFER_SESSIONS);

    /* The fifth pushed out the least recently used, the first. */
    CHECK(!xfer_auth_session(&a, tokens[0], 200));
    for (int i = 1; i <= XFER_SESSIONS; i++) CHECK(xfer_auth_session(&a, tokens[i], 200 + i));

    /* Using one keeps it: the second is now the oldest, not the first used. */
    CHECK(xfer_auth_session(&a, tokens[1], 300));
    memset(r, 0xAA, sizeof r);
    char another[XFER_TOKEN_LEN + 1];
    xfer_auth_open_session(&a, r, 301, another);
    CHECK(xfer_auth_session(&a, tokens[1], 302));
    CHECK(!xfer_auth_session(&a, tokens[2], 302));

    xfer_auth_close_session(&a, tokens[1]);
    CHECK(!xfer_auth_session(&a, tokens[1], 303));
    CHECK(xfer_auth_session_count(&a) == XFER_SESSIONS - 1);

    CHECK(!xfer_auth_session(&a, "", 0));
    CHECK(!xfer_auth_session(&a, NULL, 0));
    CHECK(!xfer_auth_session(&a, "0101", 0));

    fresh(&a);
    CHECK(xfer_auth_session_count(&a) == 0);
    CHECK(!xfer_auth_session(&a, another, 400));
}

static void test_basic(void)
{
    char pw[32];
    /* ":23AB-Z2Z6" */
    CHECK(xfer_basic_password("Basic OjIzQUItWjJaNg==", pw, sizeof pw));
    EXPECT_STR(pw, "23AB-Z2Z6");
    /* "me:pass:word" — the password may have colons */
    CHECK(xfer_basic_password("basic  bWU6cGFzczp3b3Jk", pw, sizeof pw));
    EXPECT_STR(pw, "pass:word");
    CHECK(!xfer_basic_password("Bearer abc", pw, sizeof pw));
    CHECK(!xfer_basic_password("Basic !!!", pw, sizeof pw));
    CHECK(!xfer_basic_password("Basic bm9jb2xvbg==", pw, sizeof pw));   /* "nocolon" */
    CHECK(!xfer_basic_password("Basic OjIzQUItWjJaNg==", pw, 4));
    CHECK(!xfer_basic_password(NULL, pw, sizeof pw));
}

static void test_cookie(void)
{
    char v[40];
    CHECK(xfer_cookie_value("thistle_xfer=abc", "thistle_xfer", v, sizeof v));
    EXPECT_STR(v, "abc");
    CHECK(xfer_cookie_value("a=1; thistle_xfer=def; b=2", "thistle_xfer", v, sizeof v));
    EXPECT_STR(v, "def");
    CHECK(!xfer_cookie_value("xthistle_xfer=abc", "thistle_xfer", v, sizeof v));
    CHECK(!xfer_cookie_value("thistle_xferx=abc", "thistle_xfer", v, sizeof v));
    CHECK(!xfer_cookie_value("a=1", "thistle_xfer", v, sizeof v));
    CHECK(!xfer_cookie_value("thistle_xfer=abcdef", "thistle_xfer", v, 4));
}

int main(void)
{
    test_password();
    test_check();
    test_lock();
    test_sessions();
    test_basic();
    test_cookie();
    if (s_failures) {
        fprintf(stderr, "test_xfer_auth: %d failed\n", s_failures);
        return 1;
    }
    printf("test_xfer_auth: all passed\n");
    return 0;
}
