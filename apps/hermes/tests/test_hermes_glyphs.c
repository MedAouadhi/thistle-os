// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// Hermes's text in what the fonts can draw: ASCII and Arabic as they are,
// punctuation and accented letters in ASCII, emoji as LVGL symbols or not
// at all, anything else as '?', and the output cut between characters.

#include "hermes/hermes_glyphs.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

static void expect_as(const char *in, const char *want, int line)
{
    char out[128];
    size_t n = hermes_glyphs(in, strlen(in), out, sizeof out);
    if (strcmp(out, want) != 0 || n != strlen(want)) {
        fprintf(stderr, "%s:%d: \"%s\" became \"%s\", not \"%s\"\n", __FILE__, line, in, out, want);
        s_failures++;
    }
}

#define AS(in, want) expect_as(in, want, __LINE__)

int main(void)
{
    AS("plain ASCII, 20\xC2\xB0" "C \xE2\x80\xA2 ok\n", "plain ASCII, 20\xC2\xB0" "C \xE2\x80\xA2 ok\n");
    /* Arabic for the UI's own Arabic support, and its shaped forms. */
    AS("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xEF\xBB\xBB", "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xEF\xBB\xBB");
    /* Hermes's punctuation. */
    AS("It\xE2\x80\x99s \xE2\x80\x9C" "fine\xE2\x80\x9D \xE2\x80\x94 really\xE2\x80\xA6",
       "It's \"fine\" - really...");
    AS("a \xE2\x86\x92 b, 5 \xE2\x89\xA4 6, 3\xC3\x97" "4, 10\xE2\x82\xAC", "a -> b, 5 <= 6, 3x4, 10EUR");
    AS("Gr\xC3\xB6\xC3\x9F" "e, \xC3\x86r\xC3\xB8, \xC5\x81\xC3\xB3" "d\xC5\xBA, \xC5\x92uvre",
       "Grosse, AEro, Lodz, OEuvre");
    /* Emoji with a symbol, without one, and the parts they are made of. */
    AS("\xE2\x9C\x85 done \xE2\x9D\x8C no \xE2\x9A\xA0\xEF\xB8\x8F careful",
       "\xEF\x80\x8C done \xEF\x80\x8D no \xEF\x81\xB1 careful");
    AS("price \xE2\x96\xB2 then \xF0\x9F\x94\xBB", "price \xEF\x81\xB7 then \xEF\x81\xB8");
    AS("hi \xF0\x9F\x91\x8B\xF0\x9F\x8F\xBD there \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x92\xBB!",
       "hi  there !");
    /* What no font here has, and what is not UTF-8. */
    AS("\xD0\x9F\xD1\x80\xD0\xB8 \xE4\xBD\xA0", "??? ?");
    AS("bad \xC3 \xE2\x82 \xFF end", "bad ? ? ? end");
    AS("over\xC0\xAFlong", "over?long");
    AS("tab\there\x01", "tab    here");
    AS("\xEF\xBB\xBFstart", "start");
    /* LVGL's symbols pass; another private-use character does not. */
    AS("\xEF\x80\xA1 running \xEF\x80\x8C", "\xEF\x80\xA1 running \xEF\x80\x8C");
    AS("\xEF\x80\x80", "?");

    /* Cut between characters, never inside one. */
    char out[6];
    size_t n = hermes_glyphs("ab\xD8\xA8\xD8\xA8", 6, out, sizeof out);
    if (n != 4 || strcmp(out, "ab\xD8\xA8") != 0) {
        fprintf(stderr, "%s:%d: cut inside a character: %zu\n", __FILE__, __LINE__, n);
        s_failures++;
    }
    n = hermes_glyphs("abc\xE2\x80\xA6", 6, out, 4);
    if (n != 3 || strcmp(out, "abc") != 0) {
        fprintf(stderr, "%s:%d: an ellipsis split: %zu\n", __FILE__, __LINE__, n);
        s_failures++;
    }

    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_hermes_glyphs: all checks passed\n");
    return 0;
}
