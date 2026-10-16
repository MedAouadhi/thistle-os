/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Hermes's text, in characters this device can draw. See
 * hermes_glyphs.h.
 */
#include "hermes/hermes_glyphs.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* LVGL's symbols (lv_symbol_def.h), which every Montserrat in the build
 * carries: OK, CLOSE, WARNING, UP, DOWN. */
#define SYM_OK      "\xEF\x80\x8C"
#define SYM_CLOSE   "\xEF\x80\x8D"
#define SYM_WARNING "\xEF\x81\xB1"
#define SYM_UP      "\xEF\x81\xB7"
#define SYM_DOWN    "\xEF\x81\xB8"

typedef struct {
    uint32_t cp;
    const char *as;
} swap_t;

/* Sorted by code point, for the search below. "" drops the character. */
static const swap_t SWAPS[] = {
    { 0x00A0, " " },  { 0x00A9, "(c)" }, { 0x00AB, "\"" }, { 0x00AD, "" },
    { 0x00AE, "(R)" }, { 0x00B1, "+/-" }, { 0x00B4, "'" }, { 0x00B7, "-" },
    { 0x00BB, "\"" }, { 0x00D7, "x" },   { 0x00F7, "/" },
    { 0x2002, " " },  { 0x2003, " " },   { 0x2007, " " },  { 0x2009, " " },
    { 0x200A, " " },  { 0x200B, "" },    { 0x200C, "" },   { 0x200D, "" },
    { 0x200E, "" },   { 0x200F, "" },    { 0x2010, "-" },  { 0x2011, "-" },
    { 0x2012, "-" },  { 0x2013, "-" },   { 0x2014, "-" },  { 0x2015, "-" },
    { 0x2018, "'" },  { 0x2019, "'" },   { 0x201A, "'" },  { 0x201B, "'" },
    { 0x201C, "\"" }, { 0x201D, "\"" },  { 0x201E, "\"" }, { 0x201F, "\"" },
    { 0x2026, "..." }, { 0x2027, "-" },  { 0x202F, " " },  { 0x2032, "'" },
    { 0x2033, "\"" }, { 0x2043, "-" },   { 0x20AC, "EUR" }, { 0x20E3, "" },
    { 0x2122, "TM" }, { 0x2190, "<-" },  { 0x2191, "^" },  { 0x2192, "->" },
    { 0x2193, "v" },  { 0x2194, "<->" }, { 0x21D0, "<=" }, { 0x21D2, "=>" },
    { 0x2212, "-" },  { 0x2248, "~" },   { 0x2260, "!=" }, { 0x2264, "<=" },
    { 0x2265, ">=" }, { 0x25B2, SYM_UP }, { 0x25B3, SYM_UP }, { 0x25BC, SYM_DOWN },
    { 0x25BD, SYM_DOWN }, { 0x25CF, "\xE2\x80\xA2" }, { 0x25E6, "-" },
    { 0x26A0, SYM_WARNING }, { 0x2705, SYM_OK }, { 0x2713, SYM_OK }, { 0x2714, SYM_OK },
    { 0x2717, SYM_CLOSE }, { 0x2718, SYM_CLOSE }, { 0x274C, SYM_CLOSE }, { 0x274E, SYM_CLOSE },
    { 0x2B50, "*" },  { 0xFE0E, "" },    { 0xFE0F, "" },   { 0xFEFF, "" },
    { 0x1F53A, SYM_UP }, { 0x1F53B, SYM_DOWN },
};

/* U+00C0 to U+00FF, then U+0100 to U+017F, as the letter without its mark;
 * '\0' where there is none (the two signs in the first block). */
static const char LATIN1[] = "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs"
                             "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
static const char EXTENDED_A[] = "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg"
                                 "GgGgHhHhIiIiIiIi" "IiJjJjKkkLlLlLlL"
                                 "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs"
                                 "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";

static const char *swap_for(uint32_t cp)
{
    size_t lo = 0, hi = sizeof(SWAPS) / sizeof(SWAPS[0]);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (SWAPS[mid].cp == cp) return SWAPS[mid].as;
        if (SWAPS[mid].cp < cp) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

/* What an emoji is made of, which this device has no face for. */
static bool emoji(uint32_t cp)
{
    return (cp >= 0x1F000 && cp <= 0x1FAFF) ||   /* pictographs, transport, faces */
           (cp >= 0x2600 && cp <= 0x27BF) ||     /* symbols and dingbats */
           (cp >= 0x2B00 && cp <= 0x2BFF) ||     /* arrows and stars */
           (cp >= 0xE0020 && cp <= 0xE007F) ||   /* tags, as flags use */
           (cp >= 0xFE00 && cp <= 0xFE0F) ||     /* variation selectors */
           (cp >= 0x2300 && cp <= 0x23FF);       /* watches and hourglasses */
}

/* LVGL's symbols, as every Montserrat in the build carries them (the -r
 * list at the top of lv_font_montserrat_14.c); the app draws its own marks
 * with them. Sorted. */
static const uint16_t SYMBOLS[] = {
    61441, 61448, 61451, 61452, 61453, 61457, 61459, 61461, 61465, 61468, 61473, 61478,
    61479, 61480, 61502, 61507, 61512, 61515, 61516, 61517, 61521, 61522, 61523, 61524,
    61543, 61544, 61550, 61552, 61553, 61556, 61559, 61560, 61561, 61563, 61587, 61589,
    61636, 61637, 61639, 61641, 61664, 61671, 61674, 61683, 61724, 61732, 61787, 61931,
    62016, 62017, 62018, 62019, 62020, 62087, 62099, 62189, 62212, 62810, 63426, 63650,
};

static bool symbol(uint32_t cp)
{
    size_t lo = 0, hi = sizeof(SYMBOLS) / sizeof(SYMBOLS[0]);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (SYMBOLS[mid] == cp) return true;
        if (SYMBOLS[mid] < cp) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

/* The blocks the UI's Arabic fonts cover; U+FEFF, at the end of the last,
 * is the byte order mark rather than a letter. */
static bool arabic(uint32_t cp)
{
    return (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0xFB50 && cp <= 0xFBFF) ||
           (cp >= 0xFE70 && cp <= 0xFEFC);
}

/* Decodes one character, or returns 0 bytes taken for a bad one. */
static size_t decode(const unsigned char *p, size_t len, uint32_t *cp)
{
    unsigned char c = p[0];
    size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
             : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (n == 0 || n > len) return 0;
    uint32_t v = n == 1 ? c : n == 2 ? (c & 0x1Fu) : n == 3 ? (c & 0x0Fu) : (c & 0x07u);
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (p[i] & 0x3Fu);
    }
    static const uint32_t SMALLEST[] = { 0, 0, 0x80, 0x800, 0x10000 };
    if (v < SMALLEST[n] || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
    *cp = v;
    return n;
}

static bool put(char *out, size_t cap, size_t *used, const char *s, size_t n)
{
    if (*used + n + 1 > cap) return false;
    memcpy(out + *used, s, n);
    *used += n;
    return true;
}

size_t hermes_glyphs(const char *in, size_t len, char *out, size_t cap)
{
    if (cap == 0) return 0;
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)in;
    size_t at = 0;
    while (at < len && p[at]) {
        uint32_t cp = 0;
        size_t n = decode(p + at, len - at, &cp);
        const char *raw = in + at;
        if (!n) {
            /* One '?' for a broken sequence, its continuation bytes too. */
            at++;
            for (int k = 0; k < 3 && at < len && (p[at] & 0xC0) == 0x80; k++) at++;
            if (!put(out, cap, &used, "?", 1)) break;
            continue;
        }
        at += n;

        bool ok = true;
        if (cp == '\t') {
            ok = put(out, cap, &used, "    ", 4);
        } else if (cp == '\n' || (cp >= 0x20 && cp < 0x7F) || cp == 0xB0 || cp == 0x2022 ||
                   arabic(cp) || symbol(cp)) {
            ok = put(out, cap, &used, raw, n);
        } else if (cp < 0x20 || cp == 0x7F) {
            /* Control characters draw as nothing at all. */
        } else if (swap_for(cp)) {
            const char *as = swap_for(cp);
            ok = put(out, cap, &used, as, strlen(as));
        } else if (cp >= 0xC0 && cp <= 0x17F) {
            const char *table = cp < 0x100 ? LATIN1 : EXTENDED_A;
            char letter = table[cp < 0x100 ? cp - 0xC0 : cp - 0x100];
            if (cp == 0xC6 || cp == 0xE6) ok = put(out, cap, &used, cp == 0xC6 ? "AE" : "ae", 2);
            else if (cp == 0xDF) ok = put(out, cap, &used, "ss", 2);
            else if (cp == 0xDE || cp == 0xFE) ok = put(out, cap, &used, cp == 0xDE ? "Th" : "th", 2);
            else if (cp == 0x152 || cp == 0x153) ok = put(out, cap, &used, cp == 0x152 ? "OE" : "oe", 2);
            else if (cp == 0x132 || cp == 0x133) ok = put(out, cap, &used, cp == 0x132 ? "IJ" : "ij", 2);
            else ok = put(out, cap, &used, &letter, 1);
        } else if (emoji(cp) || (cp >= 0x1F3FB && cp <= 0x1F3FF)) {
            /* Dropped, so a base glyph beside it stands alone. */
        } else {
            ok = put(out, cap, &used, "?", 1);
        }
        if (!ok) break;
    }
    out[used] = '\0';
    return used;
}
