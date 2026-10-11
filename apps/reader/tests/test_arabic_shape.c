// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// The Reader's Arabic shaping: each letter in the form its neighbours give
// it, as a font with Unicode's presentation forms draws joined script.

#include "reader/arabic_shape.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

static void check_shape(const char *in, const char *expected, const char *what)
{
    size_t len = 0;
    char *out = arabic_shape(in, strlen(in), NULL, 0, &len);
    if (!out || strcmp(out, expected) != 0 || len != strlen(expected)) {
        fprintf(stderr, "%s: shaped wrong\n  expected:", what);
        for (const char *p = expected; *p; p++) fprintf(stderr, " %02X", (unsigned char)*p);
        fprintf(stderr, "\n  actual:  ");
        for (const char *p = out ? out : ""; *p; p++) fprintf(stderr, " %02X", (unsigned char)*p);
        fprintf(stderr, "\n");
        s_failures++;
    }
    free(out);
}

int main(void)
{
    /* Initial meem, final reh (which joins nothing after it), so initial
     * hah, medial beh, final alef. */
    check_shape("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7",
                "\xEF\xBB\xA3\xEF\xBA\xAE\xEF\xBA\xA3\xEF\xBA\x92\xEF\xBA\x8E", "marhaba");
    /* Lam and alef make one ligature, final after seen, and meem after it
     * stands alone. */
    check_shape("\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85", "\xEF\xBA\xB3\xEF\xBB\xBC\xEF\xBB\xA1", "salam");
    check_shape("\xD9\x84\xD8\xA7", "\xEF\xBB\xBB", "lam-alef");
    /* Alef with madda, which LVGL's table leaves out. */
    check_shape("\xD8\xA2", "\xEF\xBA\x81", "madda");
    /* A vowel mark sits on its letter without breaking the join. */
    check_shape("\xD8\xA8\xD9\x8E\xD8\xA8", "\xEF\xBA\x91\xD9\x8E\xEF\xBA\x90", "vowel mark");
    /* Tatweel joins both ways and keeps its own code point. */
    check_shape("\xD8\xA8\xD9\x80\xD8\xA8", "\xEF\xBA\x91\xD9\x80\xEF\xBA\x90", "tatweel");
    /* A Persian zero joins nothing: it stays a zero, not U+06EF. */
    check_shape("\xDB\xB0\xD8\xA8", "\xDB\xB0\xEF\xBA\x8F", "persian digit");
    /* Other scripts, white space and a stray byte go through as they are. */
    check_shape("abc \xD9\x84\xD8\xA7 1\n2", "abc \xEF\xBB\xBB 1\n2", "mixed");
    check_shape("\xC3\xD8\xA8", "\xC3\xEF\xBA\x8F", "stray byte");
    check_shape("", "", "empty");

    /* Positions keep their place: the start of each paragraph of
     * "salam\n\nmarhaba\n\nab", and one past the end. */
    {
        static const char IN[] = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85\n\n"
                                 "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7\n\nab";
        size_t offsets[] = {32, 10, 0, 22};
        size_t len = 0;
        char *out = arabic_shape(IN, sizeof(IN) - 1, offsets, 4, &len);
        /* salam 8 bytes -> 9; marhaba 10 -> 15. */
        if (!out || offsets[0] != len || offsets[1] != 11 || offsets[2] != 0 ||
            offsets[3] != 28 || strcmp(out + 28, "ab") != 0) {
            fprintf(stderr, "offsets: %zu %zu %zu %zu, len %zu\n",
                    offsets[0], offsets[1], offsets[2], offsets[3], len);
            s_failures++;
        }
        free(out);
    }

    if (arabic_present("plain text", 10)) s_failures++;
    if (!arabic_present("x \xD8\xA8", 4)) s_failures++;

    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_arabic_shape: all checks passed\n");
    return 0;
}
