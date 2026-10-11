/*
 * SPDX-License-Identifier: MIT
 * ThistleOS — Arabic letters in the forms they take joined to their
 * neighbours.
 *
 * The table and the rules are LVGL's lv_text_ap.c, Copyright (c) 2021 LVGL
 * Kft, MIT; see THIRD_PARTY_LICENSES.md. LVGL applies them inside
 * lv_label_set_text() when LV_USE_ARABIC_PERSIAN_CHARS is set, allocating 8
 * bytes per character of the label's text from its fixed 64 KB pool without
 * checking either allocation: a note of a few KB in Notes would crash the
 * board. So the setting stays off and the Reader shapes its own text, here, in
 * one pass with no per-character arrays.
 *
 * Changes from LVGL's: alef with madda (U+0622) is in the table, so it joins,
 * and lam with it makes its ligature; LVGL's entry for plain alef was
 * labelled as madda. Lam and alef with a vowel mark between them are left as
 * two letters, where LVGL skipped the mark and then shaped the alef twice.
 * Characters that join nothing, the Persian digits, are left as they are.
 */
#include "reader/arabic_shape.h"
#include "reader/epub_port.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ALPHABET_BASE 0x0622
#define NOT_ARABIC    UINT32_MAX

typedef struct {
    uint8_t  offset;          /* from ALPHABET_BASE */
    uint16_t end_form;        /* the final form; the others are offsets from it */
    int8_t   beginning;
    int8_t   middle;
    int8_t   isolated;
    uint8_t  joins_previous;  /* joins the letter before it */
    uint8_t  joins_next;      /* joins the letter after it */
} letter_t;

static const letter_t LETTERS[] = {
    /* {Key Offset, End, Beginning, Middle, Isolated, {conjunction}} */
    {0, 0xFE82, -1, 0, -1,  1, 0},     // آ
    {1, 0xFE84, -1, 0, -1,  1, 0},    // أ
    {2, 0xFE86, -1, 0, -1,  1, 0},    // ؤ
    {3, 0xFE88, -1, 0, -1,  1, 0},    // ﺇ
    {4, 0xFE8A, 1, 2, -1,  1, 1},    // ئ
    {5, 0xFE8E, -1, 0, -1,  1, 0},    // ا
    {6, 0xFE90, 1, 2, -1,  1, 1},    // ب
    {92, 0xFB57, 1, 2, -1,  1, 1},   // پ
    {8, 0xFE96, 1, 2, -1,  1, 1},    // ت
    {9, 0xFE9A, 1, 2, -1,  1, 1},    // ث
    {10, 0xFE9E, 1, 2, -1,  1, 1},   // ج
    {100, 0xFB7B, 1, 2, -1,  1, 1},  // چ
    {11, 0xFEA2, 1, 2, -1,  1, 1},   // ح
    {12, 0xFEA6, 1, 2, -1,  1, 1},   // خ
    {13, 0xFEAA, -1, 0, -1,  1, 0},   // د
    {14, 0xFEAC, -1, 0, -1,  1, 0},   // ذ
    {15, 0xFEAE, -1, 0, -1,  1, 0},   // ر
    {16, 0xFEB0, -1, 0, -1,  1, 0},   // ز
    {118, 0xFB8B, -1, 0, -1,  1, 0},  // ژ
    {17, 0xFEB2, 1, 2, -1,  1, 1},   // س
    {18, 0xFEB6, 1, 2, -1,  1, 1},   // ش
    {19, 0xFEBA, 1, 2, -1,  1, 1},   // ص
    {20, 0xFEBE, 1, 2, -1,  1, 1},   // ض
    {21, 0xFEC2, 1, 2, -1,  1, 1},   // ط
    {22, 0xFEC6, 1, 2, -1,  1, 1},   // ظ
    {23, 0xFECA, 1, 2, -1,  1, 1},   // ع
    {24, 0xFECE, 1, 2, -1,  1, 1},   // غ
    {30, 0x0640, 0, 0, 0,  1, 1},   // - (mad, hyphen)
    {31, 0xFED2, 1, 2, -1,  1, 1},   // ف
    {32, 0xFED6, 1, 2, -1,  1, 1},   // ق
    {135, 0xFB8F, 1, 2, -1,  1, 1},  // ک
    {33, 0xFEDA, 1, 2, -1,  1, 1},  // ﻙ
    {141, 0xFB93, 1, 2, -1,  1, 1},  // گ
    {34, 0xFEDE, 1, 2, -1,  1, 1},   // ل
    {35, 0xFEE2, 1, 2, -1,  1, 1},   // م
    {36, 0xFEE6, 1, 2, -1,  1, 1},   // ن
    {38, 0xFEEE, -1, 0, -1,  1, 0},   // و
    {37, 0xFEEA, 1, 2, -1,  1, 1},   // ه
    {39, 0xFEF0, 0, 0, -1,  1, 0},   // ى
    {40, 0xFEF2, 1, 2, -1,  1, 1},   // ي
    {170, 0xFBFD, 1, 2, -1,  1, 1},   // ی
    {7, 0xFE94, -1, 2, -1,  1, 0},   // ة
    {206, 0x06F0, -1, 2, 0,  0, 0},  // ۰
    {207, 0x06F1, 0, 0, 0,  0, 0},  // ۱
    {208, 0x06F2, 0, 0, 0,  0, 0},  // ۲
    {209, 0x06F3, 0, 0, 0,  0, 0},  // ۳
    {210, 0x06F4, 0, 0, 0,  0, 0},  // ۴
    {211, 0x06F5, 0, 0, 0,  0, 0},  // ۵
    {212, 0x06F6, 0, 0, 0,  0, 0},  // ۶
    {213, 0x06F7, 0, 0, 0,  0, 0},  // ۷
    {214, 0x06F8, 0, 0, 0,  0, 0},  // ۸
    {215, 0x06F9, 0, 0, 0,  0, 0},  // ۹
};

#define LETTER_COUNT (sizeof(LETTERS) / sizeof(LETTERS[0]))

static uint32_t letter_index(uint32_t c)
{
    if (c < 0x0600 || (c > 0x06FF && c < 0xFB50)) return NOT_ARABIC;
    for (uint32_t i = 0; i < LETTER_COUNT; i++) {
        const letter_t *l = &LETTERS[i];
        if (c == (uint32_t)l->offset + ALPHABET_BASE || c == l->end_form ||
            c == (uint32_t)(l->end_form + l->beginning) ||
            c == (uint32_t)(l->end_form + l->middle) ||
            c == (uint32_t)(l->end_form + l->isolated)) {
            return i;
        }
    }
    return NOT_ARABIC;
}

static bool is_vowel_mark(uint32_t c)
{
    return c >= 0x064B && c <= 0x0652;
}

/* The lam-alef ligature for lam followed by `next`, or 0. */
static uint32_t lam_alef(uint32_t current, uint32_t next)
{
    if (current == NOT_ARABIC || next == NOT_ARABIC) return 0;
    if (LETTERS[current].offset + ALPHABET_BASE != 0x0644) return 0;
    switch (LETTERS[next].offset + ALPHABET_BASE) {
    case 0x0622: return 0xFEF5;   /* with madda */
    case 0x0623: return 0xFEF7;   /* with hamza above */
    case 0x0625: return 0xFEF9;   /* with hamza below */
    case 0x0627: return 0xFEFB;
    default:     return 0;
    }
}

/* The code point at `*pos`, which is moved past it. A byte that does not
 * begin a whole UTF-8 sequence is taken alone, and copied through unchanged. */
static uint32_t next_cp(const char *s, size_t len, size_t *pos)
{
    const unsigned char *u = (const unsigned char *)s + *pos;
    size_t left = len - *pos;
    size_t n = u[0] >= 0xF0 ? 4 : u[0] >= 0xE0 ? 3 : u[0] >= 0xC0 ? 2 : 1;
    if (n > left) n = 1;
    for (size_t k = 1; k < n; k++) {
        if ((u[k] & 0xC0) != 0x80) n = 1;
    }
    *pos += n;
    switch (n) {
    case 4:
        return (uint32_t)(u[0] & 0x07) << 18 | (uint32_t)(u[1] & 0x3F) << 12 |
               (uint32_t)(u[2] & 0x3F) << 6 | (u[3] & 0x3F);
    case 3:
        return (uint32_t)(u[0] & 0x0F) << 12 | (uint32_t)(u[1] & 0x3F) << 6 | (u[2] & 0x3F);
    case 2:
        return (uint32_t)(u[0] & 0x1F) << 6 | (u[1] & 0x3F);
    default:
        return u[0];
    }
}

/* A shaped form: a presentation form or ligature, or tatweel, all below
 * U+10000. */
static size_t put_form(char *out, uint32_t c)
{
    if (c < 0x800) {
        out[0] = (char)(0xC0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | c >> 12);
    out[1] = (char)(0x80 | (c >> 6 & 0x3F));
    out[2] = (char)(0x80 | (c & 0x3F));
    return 3;
}

bool arabic_present(const char *text, size_t len)
{
    /* U+0600..U+06FF is encoded with lead bytes D8..DB. */
    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)text[i];
        if (b >= 0xD8 && b <= 0xDB) return true;
    }
    return false;
}

/* A code point and where its bytes are. */
typedef struct {
    uint32_t cp;
    size_t   start;
    size_t   end;
} slot_t;

static slot_t read_slot(const char *text, size_t len, size_t *pos)
{
    slot_t s = {.cp = 0, .start = *pos, .end = *pos};
    if (*pos < len) {
        s.cp  = next_cp(text, len, pos);
        s.end = *pos;
    }
    return s;
}

/* Moves the offsets at or before input position `at` to output position `o`,
 * and returns the smallest still to move. */
static size_t move_offsets(size_t *offsets, bool *moved, size_t count, size_t at, size_t o)
{
    size_t next = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        if (moved[i]) continue;
        if (offsets[i] <= at) {
            offsets[i] = o;
            moved[i] = true;
        } else if (offsets[i] < next) {
            next = offsets[i];
        }
    }
    return next;
}

char *arabic_shape(const char *text, size_t len, size_t *offsets, size_t offset_count,
                   size_t *len_out)
{
    /* A shaped letter takes three bytes where it took two, and a ligature
     * three for four; everything else is copied: at most half as long again. */
    char *out = epub_malloc(len + len / 2 + 1);
    bool *moved = offset_count ? epub_malloc(offset_count * sizeof(bool)) : NULL;
    if (!out || (offset_count && !moved)) {
        epub_free(out);
        epub_free(moved);
        return NULL;
    }
    if (moved) memset(moved, 0, offset_count * sizeof(bool));
    size_t next_offset = offset_count ? 0 : SIZE_MAX;

    size_t o = 0;
    size_t pos = 0;
    uint32_t previous = NOT_ARABIC;   /* the letter before, for joining */

    /* The code point being written and the two after it: the second for
     * looking past a vowel mark to the letter it sits on. */
    slot_t w[3];
    for (int k = 0; k < 3; k++) w[k] = read_slot(text, len, &pos);

    while (w[0].end > w[0].start) {
        if (w[0].start >= next_offset) {
            next_offset = move_offsets(offsets, moved, offset_count, w[0].start, o);
        }
        uint32_t cur = w[0].cp;
        uint32_t nxt = w[1].cp;
        uint32_t shaped = 0;
        int consumed = 1;

        if (!is_vowel_mark(cur)) {
            uint32_t ci = letter_index(cur);
            if (ci == NOT_ARABIC || (!LETTERS[ci].joins_previous && !LETTERS[ci].joins_next)) {
                /* Not a letter, or one that never joins, as the Persian
                 * digits: LVGL's table would give a zero followed by a
                 * letter the "initial form" U+06EF, a different letter. */
                previous = NOT_ARABIC;
            } else {
                uint32_t ni = letter_index(is_vowel_mark(nxt) ? w[2].cp : nxt);
                bool join_prev = previous != NOT_ARABIC && LETTERS[previous].joins_next;
                bool join_next = ni != NOT_ARABIC && LETTERS[ni].joins_previous;
                uint32_t ligature = is_vowel_mark(nxt) ? 0 : lam_alef(ci, ni);
                if (ligature) {
                    shaped   = ligature + (join_prev ? 1 : 0);
                    consumed = 2;
                    previous = NOT_ARABIC;
                } else {
                    const letter_t *l = &LETTERS[ci];
                    int form = join_prev && join_next   ? l->middle
                             : !join_prev && join_next  ? l->beginning
                             : join_prev && !join_next  ? 0
                                                        : l->isolated;
                    shaped   = (uint32_t)(l->end_form + form);
                    previous = ci;
                }
            }
        }

        if (shaped) {
            o += put_form(out + o, shaped);
        } else {
            memcpy(out + o, text + w[0].start, w[0].end - w[0].start);
            o += w[0].end - w[0].start;
        }

        for (int k = 0; k < consumed; k++) {
            w[0] = w[1];
            w[1] = w[2];
            w[2] = read_slot(text, len, &pos);
        }
    }

    /* Any left are at or past the end. */
    if (moved) {
        move_offsets(offsets, moved, offset_count, SIZE_MAX, o);
        epub_free(moved);
    }
    out[o] = '\0';
    if (len_out) *len_out = o;
    return out;
}
