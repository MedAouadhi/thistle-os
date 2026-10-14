/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Podcasts app's text.
 */
#include "podcasts/pod_text.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Where to cut `s` to at most `max` bytes without splitting a character. */
static size_t utf8_cut(const char *s, size_t len, size_t max)
{
    if (len <= max) return len;
    size_t n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

void pod_text_copy(char *out, size_t cap, const char *src)
{
    if (!cap) return;
    if (!src) src = "";
    size_t n = utf8_cut(src, strlen(src), cap - 1);
    memmove(out, src, n);
    out[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* HTML                                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *out;
    size_t cap;
    size_t len;
    bool   full;
    int    pending_breaks;  /* newlines owed before the next text, 0-2 */
    bool   pending_space;
} writer_t;

/* What the next text is owed first, a break or a space, unless it starts
 * the output. */
static size_t separator(const writer_t *w, const char **sep)
{
    *sep = "";
    if (w->len == 0) return 0;
    if (w->pending_breaks) {
        *sep = "\n\n";
        return (size_t)w->pending_breaks;
    }
    if (w->pending_space) {
        *sep = " ";
        return 1;
    }
    return 0;
}

/* Text after its separator, both or neither: as much of it as fits, cut
 * between characters, and once anything is cut, nothing more. */
static void put_text(writer_t *w, const char *s, size_t n)
{
    if (w->full) return;
    const char *sep;
    size_t sep_len = separator(w, &sep);
    size_t room = w->cap - 1 - w->len;
    size_t fit = n;
    if (sep_len + n > room) {
        fit = room > sep_len ? utf8_cut(s, n, room - sep_len) : 0;
        w->full = true;
        if (fit == 0) return;
    }
    memcpy(w->out + w->len, sep, sep_len);
    memcpy(w->out + w->len + sep_len, s, fit);
    w->len += sep_len + fit;
    w->pending_breaks = 0;
    w->pending_space = false;
}

static void put_cp(writer_t *w, unsigned cp)
{
    char buf[4];
    size_t n;
    if (cp < 0x80) {
        buf[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | cp >> 6);
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | cp >> 12);
        buf[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        buf[0] = (char)(0xF0 | cp >> 18);
        buf[1] = (char)(0x80 | (cp >> 12 & 0x3F));
        buf[2] = (char)(0x80 | (cp >> 6 & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    put_text(w, buf, n);
}

static const struct {
    const char *name;
    unsigned cp;
} ENTITIES[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
    {"nbsp", ' '}, {"rsquo", 0x2019}, {"lsquo", 0x2018}, {"rdquo", 0x201D},
    {"ldquo", 0x201C}, {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013},
    {"bull", 0x2022}, {"middot", 0xB7}, {"copy", 0xA9}, {"reg", 0xAE},
    {"trade", 0x2122}, {"eacute", 0xE9}, {"egrave", 0xE8}, {"aacute", 0xE1},
    {"agrave", 0xE0}, {"ouml", 0xF6}, {"uuml", 0xFC}, {"auml", 0xE4},
    {"ccedil", 0xE7}, {"ntilde", 0xF1}, {"szlig", 0xDF}, {"euro", 0x20AC},
};

/* Decodes the entity at `p` (just past '&') into `*cp`: its length up to and
 * including ';', or 0 if it is not one. */
static size_t entity(const char *p, unsigned *cp)
{
    /* The longest name here is 6 letters; "#x10FFFF" is 8. */
    size_t n = 0;
    while (n < 12 && p[n] && p[n] != ';') n++;
    if (n == 0 || p[n] != ';') return 0;
    if (p[0] == '#') {
        unsigned v = 0;
        bool hex = n > 1 && (p[1] == 'x' || p[1] == 'X');
        size_t i = hex ? 2 : 1;
        if (i >= n) return 0;
        for (; i < n; i++) {
            char c = p[i];
            int d = c >= '0' && c <= '9' ? c - '0'
                  : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0 || v > 0x10FFFF) return 0;
            v = v * (hex ? 16 : 10) + (unsigned)d;
        }
        if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) v = 0xFFFD;
        if (v == 0xA0) v = ' ';
        *cp = v;
        return n + 1;
    }
    for (size_t i = 0; i < sizeof ENTITIES / sizeof ENTITIES[0]; i++) {
        if (strlen(ENTITIES[i].name) == n && strncmp(p, ENTITIES[i].name, n) == 0) {
            *cp = ENTITIES[i].cp;
            return n + 1;
        }
    }
    return 0;
}

/* Whether the tag at `p` (just past '<', and past any '/') is `name`. */
static bool tag_is(const char *p, const char *name)
{
    size_t n = strlen(name);
    return strncasecmp(p, name, n) == 0 && !isalnum((unsigned char)p[n]);
}

void pod_text_from_html(const char *html, char *out, size_t cap)
{
    if (!cap) return;
    writer_t w = {.out = out, .cap = cap};
    const char *p = html ? html : "";

    while (*p && !w.full) {
        unsigned char c = (unsigned char)*p;
        if (c == '<' && !(isalpha((unsigned char)p[1]) || p[1] == '/' || p[1] == '!' || p[1] == '?')) {
            /* Not a tag, as a browser reads it: "I <3 cats". */
            put_text(&w, "<", 1);
            p++;
            continue;
        }
        if (c == '<') {
            const char *name = p + 1;
            if (*name == '/') name++;
            /* Blocks end a paragraph; a <br> or a list item ends a line. */
            if (tag_is(name, "p") || tag_is(name, "div") || tag_is(name, "ul") ||
                tag_is(name, "ol") || tag_is(name, "h1") || tag_is(name, "h2") ||
                tag_is(name, "h3") || tag_is(name, "h4") || tag_is(name, "blockquote")) {
                w.pending_breaks = 2;
            } else if (tag_is(name, "br") || tag_is(name, "li") || tag_is(name, "tr")) {
                if (w.pending_breaks < 1) w.pending_breaks = 1;
            }
            /* Script and style say nothing to a reader. */
            const char *skip_to = NULL;
            if (tag_is(p + 1, "script")) skip_to = "</script";
            else if (tag_is(p + 1, "style")) skip_to = "</style";
            if (skip_to) {
                const char *end = strstr(p, skip_to);
                p = end ? end : p + strlen(p);
            }
            const char *gt = strchr(p, '>');
            if (!gt) break;
            p = gt + 1;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            w.pending_space = true;
            p++;
            continue;
        }
        if (c == '&') {
            unsigned cp;
            size_t n = entity(p + 1, &cp);
            if (n) {
                p += 1 + n;
                if (cp == ' ') w.pending_space = true;
                else put_cp(&w, cp);
                continue;
            }
        }
        /* A run of plain text, whole characters at a time. */
        const char *start = p;
        while (*p && *p != '<' && *p != '&' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            p++;
        }
        if (p == start) p++;  /* a lone '&' */
        put_text(&w, start, (size_t)(p - start));
    }
    out[w.len] = '\0';
}

/* ------------------------------------------------------------------ */
/* Durations, dates, sizes                                              */
/* ------------------------------------------------------------------ */

void pod_text_clock(int32_t seconds, char *out, size_t cap)
{
    if (seconds < 0) seconds = 0;
    int h = seconds / 3600, m = seconds / 60 % 60, s = seconds % 60;
    if (h) snprintf(out, cap, "%d:%02d:%02d", h, m, s);
    else snprintf(out, cap, "%d:%02d", m, s);
}

void pod_text_duration(int32_t seconds, char *out, size_t cap)
{
    if (seconds <= 0) {
        if (cap) out[0] = '\0';
        return;
    }
    if (seconds < 60) {
        snprintf(out, cap, "%d s", (int)seconds);
        return;
    }
    int minutes = (seconds + 30) / 60;
    int h = minutes / 60, m = minutes % 60;
    if (!h) snprintf(out, cap, "%d min", m);
    else if (!m) snprintf(out, cap, "%d h", h);
    else snprintf(out, cap, "%d h %d min", h, m);
}

static const char *const MONTHS[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

/* Days since the epoch of the local date `t` falls on. */
static long local_day(const struct tm *t)
{
    /* Days from civil, after Howard Hinnant's. */
    int y = t->tm_year + 1900 - (t->tm_mon < 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int mp = (t->tm_mon + 9) % 12;
    int doy = (153 * mp + 2) / 5 + t->tm_mday - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long)era * 146097 + doe - 719468;
}

void pod_text_date(int64_t when, int64_t now, char *out, size_t cap)
{
    if (when <= 0) {
        if (cap) out[0] = '\0';
        return;
    }
    time_t w = (time_t)when, n = (time_t)now;
    struct tm tw, tn;
    localtime_r(&w, &tw);
    localtime_r(&n, &tn);
    long days = local_day(&tn) - local_day(&tw);
    if (days == 0) snprintf(out, cap, "Today");
    else if (days == 1) snprintf(out, cap, "Yesterday");
    else if (days > 1 && days < 7) snprintf(out, cap, "%ld days ago", days);
    else if (tw.tm_year == tn.tm_year) snprintf(out, cap, "%d %s", tw.tm_mday, MONTHS[tw.tm_mon]);
    else snprintf(out, cap, "%d %s %d", tw.tm_mday, MONTHS[tw.tm_mon], tw.tm_year + 1900);
}

void pod_text_size(int64_t bytes, char *out, size_t cap)
{
    if (bytes < 0) bytes = 0;
    if (bytes < 1000 * 1000) {
        snprintf(out, cap, "%d KB", (int)((bytes + 999) / 1000));
    } else if (bytes < 1000LL * 1000 * 1000) {
        snprintf(out, cap, "%d MB", (int)((bytes + 500000) / 1000000));
    } else {
        int tenths = (int)((bytes + 50000000) / 100000000);
        snprintf(out, cap, "%d.%d GB", tenths / 10, tenths % 10);
    }
}

int pod_text_url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t len = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        bool plain = isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        size_t n = plain ? 1 : 3;
        if (len + n + 1 > cap) {
            if (cap) out[0] = '\0';
            return 0;
        }
        if (plain) {
            out[len++] = (char)*p;
        } else {
            out[len++] = '%';
            out[len++] = hex[*p >> 4];
            out[len++] = hex[*p & 15];
        }
    }
    if (cap) out[len] = '\0';
    return 1;
}
