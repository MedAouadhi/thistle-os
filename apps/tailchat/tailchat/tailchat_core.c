/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Chat app's text, with no LVGL in it
 */
#include "tailchat/tailchat_core.h"

#include <stdio.h>
#include <string.h>

#define ELLIPSIS "\xe2\x80\xa6"

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
