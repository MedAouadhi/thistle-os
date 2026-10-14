// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// SHA-1 against FIPS 180's examples, at the block boundaries the padding
// turns on, and PodcastIndex's Authorization header from its own example.

#include "podcasts/pod_sha1.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

static void check(const char *text, const char *expected, int line)
{
    char out[41];
    pod_sha1_hex(text, out);
    if (strcmp(out, expected) == 0) return;
    fprintf(stderr, "%s:%d: sha1(\"%.40s%s\")\n  expected: %s\n  actual:   %s\n", __FILE__, line,
            text, strlen(text) > 40 ? "..." : "", expected, out);
    s_failures++;
}

#define CHECK(text, expected) check((text), (expected), __LINE__)

int main(void)
{
    CHECK("", "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK("abc", "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK("The quick brown fox jumps over the lazy dog",
          "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");

    /* 55 bytes leave room for the length in the last block; 56 do not. */
    char a[129];
    memset(a, 'a', sizeof a - 1);
    a[55] = '\0';
    CHECK(a, "c1c8bbdc22796e28c0e15163d20899b65621d65a");
    a[55] = 'a';
    a[56] = '\0';
    CHECK(a, "c2db330f6083854c99d4b5bfb6e8f29f201be699");
    a[56] = 'a';
    a[64] = '\0';
    CHECK(a, "0098ba824b5c16427bd7a1122a5a442a25ec644d");
    a[64] = 'a';
    a[128] = '\0';
    CHECK(a, "ad5b3fdbcb526778c2839d2f151ea753995e26a0");

    /* The example key, secret and time in the API's documentation. */
    CHECK("UXKCGDSYGUUEVQJSYDZHyzJe2eE7XV-3eY576dyRZ6wXyAbndh6LUrCZ8KN|1613713388",
          "73a1fffed61c1d30d858beb1fc48f355386449d2");

    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_pod_sha1: all checks passed\n");
    return 0;
}
