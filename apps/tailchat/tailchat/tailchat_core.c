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

void tailchat_row(char *out, size_t n, const char *title, const char *detail, size_t max_chars)
{
    char line[160];
    tailchat_one_line(line, sizeof line, detail, max_chars);
    if (line[0]) snprintf(out, n, "%s\n%s", title ? title : "", line);
    else snprintf(out, n, "%s", title ? title : "");
}

void tailchat_message_row(char *out, size_t n, bool mine, const char *name, const char *text,
                          size_t max_chars)
{
    char line[512];
    tailchat_one_line(line, sizeof line, text, max_chars);
    snprintf(out, n, "%s: %s", mine ? "You" : (name ? name : ""), line);
}

const char *tailchat_tab_label(tailchat_tab_t tab, int waiting, char *out, size_t n)
{
    switch (tab) {
    case TAILCHAT_TAB_CHATS:
        snprintf(out, n, "Chats");
        break;
    case TAILCHAT_TAB_CONTACTS:
        snprintf(out, n, "Contacts");
        break;
    case TAILCHAT_TAB_REQUESTS:
        if (waiting > 0) snprintf(out, n, "Requests %d", waiting);
        else snprintf(out, n, "Requests");
        break;
    }
    return out;
}

const char *tailchat_answer_text(int8_t accepted_by_them)
{
    if (accepted_by_them > 0) return "Accepted this device";
    if (accepted_by_them == 0) return "Declined this device";
    return "";
}

#define SEP ", "

const char *const tailchat_reactions[TAILCHAT_REACTIONS] = {
    "\xf0\x9f\x91\x8d",          /* thumbs up */
    "\xe2\x9d\xa4\xef\xb8\x8f",  /* red heart */
    "\xf0\x9f\x98\x82",          /* tears of joy */
    "\xf0\x9f\x8e\x89",          /* party popper */
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

static const char *state_text(const char *state)
{
    if (!state) return "";
    if (strcmp(state, "waiting") == 0) return "Waiting";
    if (strcmp(state, "delivered") == 0) return "Delivered";
    if (strcmp(state, "read") == 0) return "Read";
    return "";
}

const char *tailchat_duration(char *out, size_t n, uint32_t ms)
{
    /* Whole seconds, but never 0:00 for a short one. */
    uint32_t s = ms >= 1000 ? ms / 1000 : ms > 0;
    snprintf(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    return out;
}

void tailchat_bubble(char *out, size_t n, const tailchat_bubble_t *b, size_t max_chars)
{
    if (!out || n == 0) return;
    out[0] = '\0';
    if (!b) return;
    const char *name = b->name ? b->name : "";
    size_t at = 0;
#define PUT(...)                                                  \
    do {                                                          \
        if (at < n) {                                             \
            int w_ = snprintf(out + at, n - at, __VA_ARGS__);     \
            at += w_ > 0 ? (size_t)w_ : 0;                        \
        }                                                         \
    } while (0)

    char line[512];
    if (b->quote && b->quote[0]) {
        tailchat_one_line(line, sizeof line, b->quote, 30);
        PUT("> %s: %s\n", b->quote_mine ? "You" : name, line);
    } else if (b->quoted) {
        PUT("> an earlier message\n");
    }
    if (b->voice_ms) {
        char length[16];
        PUT("%s: Voice message %s%s", b->mine ? "You" : name, tailchat_duration(length, sizeof length, b->voice_ms),
            b->playing ? ", playing" : "");
    } else {
        tailchat_one_line(line, sizeof line, b->text, max_chars);
        PUT("%s: %s", b->mine ? "You" : name, line);
    }

    const char *sep = "\n";
    const char *st = b->mine ? state_text(b->state) : "";
    if (st[0]) {
        PUT("%s%s", sep, st);
        sep = SEP;
    }
    if (b->react_them && b->react_them[0]) {
        PUT("%s%s %s", sep, name, tailchat_emoji_label(b->react_them));
        sep = SEP;
    }
    if (b->react_me && b->react_me[0]) {
        PUT("%sYou %s", sep, tailchat_emoji_label(b->react_me));
    }
#undef PUT
    if (at >= n) out[n - 1] = '\0';
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
    if (minutes == 0) snprintf(out, n, "Check while asleep: off");
    else if (minutes % 60 == 0) snprintf(out, n, "Check while asleep: %u h", (unsigned)(minutes / 60));
    else snprintf(out, n, "Check while asleep: %u min", (unsigned)minutes);
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
