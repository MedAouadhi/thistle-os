/*
 * Unit tests for Arabic shaping: each letter in the form its neighbours give
 * it, as a font with Unicode's presentation forms draws joined script, and
 * the way back from a place in the shaped text to the letters it came from,
 * which a text field's cursor and taps are found by.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "test_runner.h"
#include "ui/arabic_shape.h"

#include <stdlib.h>
#include <string.h>

/* "مرحبا", "سلام": meem reh hah beh alef; seen lam alef meem. */
#define MARHABA "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7"
#define SALAM   "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85"

/* Whether `in` shapes to `expected`, the length said included. */
static bool shapes_to(const char *in, const char *expected)
{
    size_t len = 0;
    char *out = arabic_shape(in, strlen(in), NULL, 0, &len);
    bool ok = out && strcmp(out, expected) == 0 && len == strlen(expected);
    if (!ok) {
        printf("\n    expected:");
        for (const char *p = expected; *p; p++) printf(" %02X", (unsigned char)*p);
        printf("\n    actual:  ");
        for (const char *p = out ? out : ""; *p; p++) printf(" %02X", (unsigned char)*p);
        printf("\n");
    }
    free(out);
    return ok;
}

TEST(arabic_letters_take_their_joined_forms) {
    /* Initial meem, final reh (which joins nothing after it), so initial
     * hah, medial beh, final alef. */
    ASSERT_TRUE(shapes_to(MARHABA, "\xEF\xBB\xA3\xEF\xBA\xAE\xEF\xBA\xA3\xEF\xBA\x92\xEF\xBA\x8E"));
    /* Alef with madda, which LVGL's table leaves out. */
    ASSERT_TRUE(shapes_to("\xD8\xA2", "\xEF\xBA\x81"));
}

TEST(arabic_lam_and_alef_make_one_ligature) {
    /* Final after seen, and meem after it stands alone. */
    ASSERT_TRUE(shapes_to(SALAM, "\xEF\xBA\xB3\xEF\xBB\xBC\xEF\xBB\xA1"));
    ASSERT_TRUE(shapes_to("\xD9\x84\xD8\xA7", "\xEF\xBB\xBB"));
}

TEST(arabic_marks_and_tatweel_keep_the_join) {
    /* A vowel mark sits on its letter without breaking the join. */
    ASSERT_TRUE(shapes_to("\xD8\xA8\xD9\x8E\xD8\xA8", "\xEF\xBA\x91\xD9\x8E\xEF\xBA\x90"));
    /* Tatweel joins both ways and keeps its own code point. */
    ASSERT_TRUE(shapes_to("\xD8\xA8\xD9\x80\xD8\xA8", "\xEF\xBA\x91\xD9\x80\xEF\xBA\x90"));
    /* A Persian zero joins nothing: it stays a zero, not U+06EF. */
    ASSERT_TRUE(shapes_to("\xDB\xB0\xD8\xA8", "\xDB\xB0\xEF\xBA\x8F"));
}

TEST(arabic_shaping_leaves_everything_else_as_it_is) {
    ASSERT_TRUE(shapes_to("abc \xD9\x84\xD8\xA7 1\n2", "abc \xEF\xBB\xBB 1\n2"));
    ASSERT_TRUE(shapes_to("\xC3\xD8\xA8", "\xC3\xEF\xBA\x8F"));   /* a stray byte */
    ASSERT_TRUE(shapes_to("", ""));
}

TEST(arabic_shaping_moves_positions_with_the_text) {
    /* The start of each paragraph of "salam\n\nmarhaba\n\nab", and one past
     * the end. salam takes 8 bytes and shaped 9; marhaba 10 and 15. */
    static const char IN[] = SALAM "\n\n" MARHABA "\n\nab";
    size_t offsets[] = {32, 10, 0, 22};
    size_t len = 0;
    char *out = arabic_shape(IN, sizeof(IN) - 1, offsets, 4, &len);
    ASSERT_TRUE(out != NULL);
    bool ok = offsets[0] == len && offsets[1] == 11 && offsets[2] == 0 && offsets[3] == 28 &&
              strcmp(out + 28, "ab") == 0;
    free(out);
    ASSERT_TRUE(ok);
}

TEST(arabic_present_finds_the_arabic_block) {
    ASSERT_TRUE(!arabic_present("plain text", 10));
    ASSERT_TRUE(arabic_present("x \xD8\xA8", 4));
}

TEST(arabic_needs_shaping_only_for_letters_not_yet_joined) {
    ASSERT_TRUE(arabic_needs_shaping(SALAM, strlen(SALAM)));
    ASSERT_TRUE(arabic_needs_shaping("ok " MARHABA, strlen("ok " MARHABA)));
    ASSERT_TRUE(!arabic_needs_shaping("plain text", 10));
    ASSERT_TRUE(!arabic_needs_shaping("", 0));

    /* Text shaped already, as the Reader, Phone and Podcasts set it. */
    size_t len = 0;
    char *shaped = arabic_shape(MARHABA " " SALAM, strlen(MARHABA " " SALAM), NULL, 0, &len);
    ASSERT_TRUE(shaped != NULL);
    bool again = arabic_needs_shaping(shaped, len);
    free(shaped);
    ASSERT_TRUE(!again);

    /* Persian digits and a vowel mark alone join nothing. */
    ASSERT_TRUE(!arabic_needs_shaping("\xDB\xB1\xDB\xB2", 4));
    ASSERT_TRUE(!arabic_needs_shaping("\xD9\x8E", 2));
    /* A letter's lead byte cut off at the end is not a letter. */
    ASSERT_TRUE(!arabic_needs_shaping("\xD8", 1));
}

TEST(arabic_logical_offset_finds_the_letters_a_place_came_from) {
    /* SALAM shaped: initial seen 0..2, the lam-alef ligature 3..5, meem 6..8;
     * in the letters seen 0, lam 2, alef 4, meem 6, the end 8. */
    const size_t n = strlen(SALAM);
    ASSERT_EQ(arabic_logical_offset(SALAM, n, 0), 0);
    ASSERT_EQ(arabic_logical_offset(SALAM, n, 3), 2);
    ASSERT_EQ(arabic_logical_offset(SALAM, n, 4), 2);   /* inside the ligature */
    ASSERT_EQ(arabic_logical_offset(SALAM, n, 6), 6);
    ASSERT_EQ(arabic_logical_offset(SALAM, n, 9), 8);
    ASSERT_EQ(arabic_logical_offset(SALAM, n, 100), 8);

    /* Latin before it is copied byte for byte. */
    ASSERT_EQ(arabic_logical_offset("ab \xD9\x84\xD8\xA7", 7, 2), 2);
    ASSERT_EQ(arabic_logical_offset("ab \xD9\x84\xD8\xA7", 7, 3), 3);
    ASSERT_EQ(arabic_logical_offset("ab \xD9\x84\xD8\xA7", 7, 6), 7);
}

TEST(arabic_a_place_shaped_and_found_again_is_where_it_was) {
    /* Every character boundary of mixed text, shaped and back, as a cursor
     * goes; the alef of a lam-alef has none of its own and comes back after
     * the ligature. */
    static const char IN[] = "Hi " MARHABA ", " SALAM "!";
    const size_t n = sizeof(IN) - 1;
    for (size_t at = 0; at <= n; at++) {
        if (at < n && ((unsigned char)IN[at] & 0xC0) == 0x80) continue;
        size_t moved = at;
        char *out = arabic_shape(IN, n, &moved, 1, NULL);
        ASSERT_TRUE(out != NULL);
        free(out);
        size_t back = arabic_logical_offset(IN, n, moved);
        size_t alef = strlen("Hi " MARHABA ", ") + 4;
        ASSERT_EQ(back, at == alef ? alef + 2 : at);
    }
}
