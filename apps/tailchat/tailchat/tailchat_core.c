/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Chat app's text, with no LVGL in it
 */
#include "tailchat/tailchat_core.h"

#include <stdio.h>
#include <string.h>

#define ELLIPSIS "..."  /* Montserrat has no U+2026 */

/* The bytes of the UTF-8 character starting with `c`. */
static size_t char_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

void tailchat_one_line(char *out, size_t n, const char *text, size_t max_chars)
{
    if (!out || n == 0) return;
    out[0] = '\0';
    if (!text || max_chars == 0) return;

    size_t total = 0;
    for (const char *p = text; *p; p += char_len((unsigned char)*p)) total++;
    bool cut = total > max_chars;
    size_t keep = cut ? max_chars - 1 : total;
    size_t room = n - 1 - (cut ? sizeof ELLIPSIS - 1 : 0);
    if (cut && n - 1 < sizeof ELLIPSIS - 1) room = 0;

    size_t at = 0, chars = 0;
    for (const char *p = text; *p && chars < keep;) {
        size_t len = char_len((unsigned char)*p);
        if (strnlen(p, len) < len) break; /* a character cut short in the text */
        if (at + len > room) break;
        if (*p == '\n' || *p == '\r' || *p == '\t') {
            /* One space for a line break, however written */
            if (at > 0 && out[at - 1] != ' ') out[at++] = ' ';
        } else {
            memcpy(out + at, p, len);
            at += len;
        }
        p += len;
        chars++;
    }
    while (at > 0 && out[at - 1] == ' ') at--;
    if (cut && at + sizeof ELLIPSIS - 1 <= n - 1) {
        memcpy(out + at, ELLIPSIS, sizeof ELLIPSIS - 1);
        at += sizeof ELLIPSIS - 1;
    }
    out[at] = '\0';
}

bool tailchat_excerpt(char *out, size_t n, const char *text, size_t max_chars, size_t max_lines)
{
    if (!out || n == 0) return false;
    out[0] = '\0';
    if (!text || max_chars == 0 || max_lines == 0) return false;

    size_t total = 0;
    for (const char *p = text; *p; p += char_len((unsigned char)*p)) total += *p != '\r' && *p != '\n';
    size_t keep = total > max_chars ? max_chars - 1 : total;
    size_t room = n - 1 > sizeof ELLIPSIS - 1 ? n - 1 - (sizeof ELLIPSIS - 1) : 0;

    size_t at = 0, chars = 0, lines = 1, breaks = 0;
    bool cut = total > max_chars;
    for (const char *p = text; *p;) {
        size_t len = char_len((unsigned char)*p);
        if (strnlen(p, len) < len) break; /* a character cut short in the text */
        if (*p == '\r' || *p == '\n') {
            /* Line breaks wait until something follows them. */
            breaks += *p == '\n';
            p++;
            continue;
        }
        if (chars >= keep) break;
        size_t take = at == 0 ? 0 : breaks > 2 ? 2 : breaks;
        if (lines + take > max_lines || at + take + len > room) {
            cut = true;
            break;
        }
        for (size_t k = 0; k < take; k++) out[at++] = '\n';
        lines += take;
        breaks = 0;
        if (*p == '\t') out[at] = ' ';
        else memcpy(out + at, p, len);
        at += *p == '\t' ? 1 : len;
        p += len;
        chars++;
    }
    if (cut) {
        while (at > 0 && (out[at - 1] == ' ' || out[at - 1] == '\n')) at--;
        memcpy(out + at, ELLIPSIS, sizeof ELLIPSIS - 1);
        at += sizeof ELLIPSIS - 1;
    }
    out[at] = '\0';
    return cut;
}

const char *tailchat_answer_text(int8_t accepted_by_them)
{
    if (accepted_by_them > 0) return "Accepted this device";
    if (accepted_by_them == 0) return "Declined this device";
    return "";
}

/* The companion app's choices, in its order. */
const char *const tailchat_reactions[TAILCHAT_REACTIONS] = {
    "\xf0\x9f\x91\x8d",          /* thumbs up */
    "\xe2\x9d\xa4\xef\xb8\x8f",  /* red heart */
    "\xf0\x9f\x98\x82",          /* tears of joy */
    "\xf0\x9f\x98\xae",          /* open mouth */
    "\xf0\x9f\x98\xa2",          /* crying */
    "\xf0\x9f\x99\x8f",          /* folded hands */
};

const char *tailchat_emoji_label(const char *emoji)
{
    static const struct {
        const char *emoji;
        const char *label;
    } known[] = {
        {"\xf0\x9f\x91\x8d", "+1"},     {"\xe2\x9d\xa4", "<3"},
        {"\xf0\x9f\x98\x82", "haha"},   {"\xf0\x9f\x8e\x89", "yay"},
        {"\xf0\x9f\x98\xae", "wow"},    {"\xf0\x9f\x98\xa2", "sad"},
        {"\xf0\x9f\x99\x8f", "thanks"}, {"\xf0\x9f\x91\x8e", "-1"},
    };
    if (!emoji || !emoji[0]) return "";
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) {
        /* A heart with or without its variation selector */
        if (strncmp(emoji, known[i].emoji, strlen(known[i].emoji)) == 0) return known[i].label;
    }
    return "*";
}

const char *tailchat_duration(char *out, size_t n, uint32_t ms)
{
    /* Whole seconds, but never 0:00 for a short one. */
    uint32_t s = ms >= 1000 ? ms / 1000 : ms > 0;
    snprintf(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    return out;
}

bool tailchat_parse_address(const char *text, char *host, size_t n, uint16_t *port)
{
    if (!text || !host || n == 0 || !port) return false;
    while (*text == ' ') text++;
    size_t len = strlen(text);
    while (len > 0 && text[len - 1] == ' ') len--;
    const char *colon = memchr(text, ':', len);
    size_t host_len = colon ? (size_t)(colon - text) : len;
    if (host_len == 0 || host_len >= n || memchr(text, ' ', host_len)) return false;
    unsigned long p = 0;
    if (colon) {
        const char *d = colon + 1;
        const char *end = text + len;
        if (d == end) return false;
        for (; d < end; d++) {
            if (*d < '0' || *d > '9') return false;
            p = p * 10 + (unsigned long)(*d - '0');
            if (p > 65535) return false;
        }
        if (p == 0) return false;
    }
    memcpy(host, text, host_len);
    host[host_len] = '\0';
    *port = (uint16_t)p;
    return true;
}

static const uint32_t CHECK_CHOICES[] = {5, 15, 30, 60};

const char *tailchat_check_label(uint32_t minutes, char *out, size_t n)
{
    if (minutes == 0) snprintf(out, n, "Off");
    else if (minutes == 60) snprintf(out, n, "Every hour");
    else if (minutes % 60 == 0) snprintf(out, n, "Every %u hours", (unsigned)(minutes / 60));
    else snprintf(out, n, "Every %u min", (unsigned)minutes);
    return out;
}

uint32_t tailchat_next_check_minutes(uint32_t minutes)
{
    if (minutes == 0) return CHECK_CHOICES[0];
    for (size_t i = 0; i < sizeof CHECK_CHOICES / sizeof CHECK_CHOICES[0]; i++) {
        if (CHECK_CHOICES[i] > minutes) return CHECK_CHOICES[i];
    }
    return 0;
}

const char *tailchat_initials(const char *name, char *out, size_t n)
{
    if (!out || n == 0) return out;
    size_t at = 0;
    int words = 0;
    bool in_word = false;
    for (const char *p = name ? name : ""; *p && words < 2;) {
        unsigned char c = (unsigned char)*p;
        size_t len = char_len(c);
        if (strnlen(p, len) < len) break;
        /* A word starts at a letter or digit; ASCII punctuation and spaces
         * part words, and anything past ASCII counts as a letter. */
        bool letter = c >= 0x80 || (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z');
        if (letter && !in_word) {
            if (at + len >= n) break;
            memcpy(out + at, p, len);
            if (len == 1 && c >= 'a' && c <= 'z') out[at] = (char)(c - 'a' + 'A');
            at += len;
            words++;
        }
        /* A hyphen or a dot goes on with the word: "lena-laptop" is one. */
        in_word = letter || (in_word && (c == '-' || c == '.' || c == '_'));
        p += len;
    }
    if (at == 0 && n > 1) out[at++] = '?';
    out[at] = '\0';
    return out;
}

static const char *const DAY_SHORT[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const DAY_LONG[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
                                        "Saturday"};
static const char *const MONTH_SHORT[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const MONTH_LONG[12] = {"January", "February", "March", "April", "May", "June", "July",
                                           "August", "September", "October", "November", "December"};

bool tailchat_same_day(const struct tm *a, const struct tm *b)
{
    return a->tm_year == b->tm_year && a->tm_yday == b->tm_yday;
}

/* Days from `then` to `now` by the calendar, 0 for the same day; negative
 * for a day after now. */
static long days_before(const struct tm *now, const struct tm *then)
{
    long d = 0;
    for (int y = then->tm_year; y < now->tm_year; y++) {
        int year = y + 1900;
        d += (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;
    }
    for (int y = now->tm_year; y < then->tm_year; y++) {
        int year = y + 1900;
        d -= (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;
    }
    return d + now->tm_yday - then->tm_yday;
}

const char *tailchat_clock(const struct tm *then, bool h24, char *out, size_t n)
{
    if (h24) {
        snprintf(out, n, "%02d:%02d", then->tm_hour, then->tm_min);
    } else {
        int h = then->tm_hour % 12;
        snprintf(out, n, "%d:%02d %s", h ? h : 12, then->tm_min, then->tm_hour < 12 ? "AM" : "PM");
    }
    return out;
}

const char *tailchat_when(const struct tm *now, const struct tm *then, bool h24, char *out, size_t n)
{
    long days = days_before(now, then);
    if (days <= 0) return tailchat_clock(then, h24, out, n);
    if (days == 1) snprintf(out, n, "Yesterday");
    else if (days < 7) snprintf(out, n, "%s", DAY_SHORT[then->tm_wday % 7]);
    else if (then->tm_year == now->tm_year) snprintf(out, n, "%d %s", then->tm_mday, MONTH_SHORT[then->tm_mon % 12]);
    else snprintf(out, n, "%d %s %d", then->tm_mday, MONTH_SHORT[then->tm_mon % 12], then->tm_year + 1900);
    return out;
}

const char *tailchat_day(const struct tm *now, const struct tm *then, char *out, size_t n)
{
    long days = days_before(now, then);
    if (days <= 0) snprintf(out, n, "Today");
    else if (days == 1) snprintf(out, n, "Yesterday");
    else if (days < 7) snprintf(out, n, "%s", DAY_LONG[then->tm_wday % 7]);
    else if (then->tm_year == now->tm_year)
        snprintf(out, n, "%s, %d %s", DAY_LONG[then->tm_wday % 7], then->tm_mday, MONTH_LONG[then->tm_mon % 12]);
    else snprintf(out, n, "%d %s %d", then->tm_mday, MONTH_LONG[then->tm_mon % 12], then->tm_year + 1900);
    return out;
}

bool tailchat_joins(bool before_mine, uint64_t before_ts, bool mine, uint64_t ts)
{
    if (before_mine != mine) return false;
    uint64_t gap = ts >= before_ts ? ts - before_ts : before_ts - ts;
    return gap <= TAILCHAT_RUN_MS;
}

const char *tailchat_presence(const tailchat_presence_t *p)
{
    if (!p) return "";
    if (p->typing) return "typing...";
    if (p->pending) return "Not reached yet";
    if (p->waiting) return p->connected ? "Sending..." : "Waiting to send";
    if (p->connected) return "Online";
    return p->seen_name ? p->seen_name : "";
}

void tailchat_waveform(const char *id, uint8_t *bars, size_t count, uint8_t max)
{
    /* FNV-1a over the id, then a xorshift for each bar, smoothed a little
     * so neighbours rise and fall together as speech does. */
    uint32_t h = 2166136261u;
    for (const char *p = id ? id : ""; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    if (max == 0) max = 1;
    uint32_t prev = max / 2;
    for (size_t i = 0; i < count; i++) {
        h ^= h << 13;
        h ^= h >> 17;
        h ^= h << 5;
        uint32_t level = 1 + h % max;
        level = (level * 2 + prev) / 3;
        if (level < 1) level = 1;
        if (level > max) level = max;
        bars[i] = (uint8_t)level;
        prev = level;
    }
}

tailchat_tick_t tailchat_tick(bool mine, const char *state)
{
    if (!mine || !state) return TAILCHAT_TICK_NONE;
    if (strcmp(state, "waiting") == 0) return TAILCHAT_TICK_WAITING;
    if (strcmp(state, "delivered") == 0) return TAILCHAT_TICK_DELIVERED;
    if (strcmp(state, "read") == 0) return TAILCHAT_TICK_READ;
    return TAILCHAT_TICK_NONE;
}
