/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Phone app's words for times and numbers (phone_text.h).
 */
#include "phone/phone_text.h"

#include <stdio.h>
#include <string.h>

static const char *const WEEKDAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/* Local midnight `days_back` days before the day `now` falls on. Through
 * mktime(), so a day that is 23 or 25 hours long is still one day. */
static time_t midnight(time_t now, int days_back)
{
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_mday -= days_back;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

void phone_text_when(uint32_t when, time_t now, bool h24, char *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = '\0';
    if (when == 0) return;

    time_t t = (time_t)when;
    struct tm tm;
    localtime_r(&t, &tm);

    if (t <= now && t >= midnight(now, 0)) {
        if (h24) {
            snprintf(out, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
        } else {
            int hour = tm.tm_hour % 12;
            snprintf(out, cap, "%d:%02d %s", hour ? hour : 12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
        }
        return;
    }
    if (t <= now && t >= midnight(now, 1)) {
        snprintf(out, cap, "Yesterday");
        return;
    }
    if (t <= now && t >= midnight(now, 6)) {
        snprintf(out, cap, "%s", WEEKDAYS[tm.tm_wday % 7]);
        return;
    }
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    /* A year or more back, the month is enough, and keeps the list's
     * column of times narrow. */
    if (tm.tm_year == now_tm.tm_year) {
        snprintf(out, cap, "%d %s", tm.tm_mday, MONTHS[tm.tm_mon % 12]);
    } else {
        snprintf(out, cap, "%s %d", MONTHS[tm.tm_mon % 12], tm.tm_year + 1900);
    }
}

char phone_text_dial_char(uint32_t key)
{
    if ((key >= '0' && key <= '9') || key == '+' || key == '*' || key == '#') return (char)key;
    return 0;
}

bool phone_text_dial_append(char *number, size_t cap, char c)
{
    size_t len = strlen(number);
    if (!c || len + 1 >= cap) return false;
    number[len] = c;
    number[len + 1] = '\0';
    return true;
}

bool phone_text_dial_delete(char *number)
{
    size_t len = strlen(number);
    if (!len) return false;
    number[len - 1] = '\0';
    return true;
}

/* The length of the UTF-8 character starting with `lead`. */
static size_t utf8_len(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Whether a word beginning with the character at `s` gives an initial: a
 * letter, or any character beyond ASCII ("Ü", "م"). A number gives none. */
static bool is_letter_start(const char *s)
{
    unsigned char c = (unsigned char)*s;
    if (c >= 0x80) return true;
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static void append_initial(char *out, size_t cap, const char *s)
{
    size_t used = strlen(out);
    size_t n = utf8_len((unsigned char)*s);
    for (size_t i = 0; i < n; i++) {
        if (!s[i]) return;
    }
    if (used + n + 1 > cap) return;
    memcpy(out + used, s, n);
    if (n == 1 && out[used] >= 'a' && out[used] <= 'z') out[used] = (char)(out[used] - 'a' + 'A');
    out[used + n] = '\0';
}

void phone_text_initials(const char *name, char *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = '\0';
    if (!name) return;

    const char *first = NULL, *last = NULL;
    bool in_word = false;
    for (const char *s = name; *s; s += utf8_len((unsigned char)*s)) {
        bool space = *s == ' ' || *s == '\t' || *s == '-' || *s == ',' || *s == '(' || *s == '"';
        if (space) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            if (is_letter_start(s)) {
                if (!first) first = s;
                else last = s;
            }
        }
    }
    if (first) append_initial(out, cap, first);
    if (last) append_initial(out, cap, last);
}

bool phone_text_ascii(const char *text)
{
    for (const unsigned char *s = (const unsigned char *)text; s && *s; s++) {
        if (*s < 0x20 || *s > 0x7E) return false;
    }
    return true;
}

bool phone_text_same_number(const char *a, const char *b)
{
    if (!a || !b) return false;
    for (;;) {
        while (*a && !(*a >= '0' && *a <= '9') && *a != '+' && *a != '*' && *a != '#') a++;
        while (*b && !(*b >= '0' && *b <= '9') && *b != '+' && *b != '*' && *b != '#') b++;
        if (*a != *b) return false;
        if (!*a) return true;
        a++;
        b++;
    }
}

void phone_text_compact(const char *number, char *out, size_t cap)
{
    if (!out || !cap) return;
    size_t used = 0;
    for (const char *s = number; s && *s && used + 1 < cap; s++) {
        if (phone_text_dial_char((unsigned char)*s)) out[used++] = *s;
    }
    out[used] = '\0';
}
