/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Chat app's text, with no LVGL in it (host-tested by
 * apps/tailchat/tests/run_tests.sh).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TAILCHAT_TAB_CHATS,
    TAILCHAT_TAB_CONTACTS,
    TAILCHAT_TAB_REQUESTS,
} tailchat_tab_t;

/* `text` on one line, its line breaks made spaces, cut to at most
 * `max_chars` characters (the last "..." when cut) and to the buffer,
 * never inside a character. */
void tailchat_one_line(char *out, size_t n, const char *text, size_t max_chars);

/* A list row: the title, and below it the detail on one line. */
void tailchat_row(char *out, size_t n, const char *title, const char *detail, size_t max_chars);

/* A conversation's row: "You: ..." or "<name>: ...". */
void tailchat_message_row(char *out, size_t n, bool mine, const char *name, const char *text,
                          size_t max_chars);

/* A message in a conversation, as tailchat_bubble() writes it. */
typedef struct {
    bool        mine;
    const char *name;        /* the contact's */
    const char *text;
    const char *state;       /* "waiting", "delivered", "read", "received" */
    bool        quoted;      /* a reply: quote is what it answers, "" if no longer kept */
    const char *quote;       /* NULL or "" and !quoted: not a reply */
    bool        quote_mine;
    const char *react_me;    /* emoji or "" */
    const char *react_them;
} tailchat_bubble_t;

/* A conversation's row, on up to three lines: what a reply quotes
 * ("> Ben: tea or coffee?"), who wrote it and what ("You: tea"), and its
 * state and reactions ("Read, Ben +1"), each line only when it has
 * something. The text is cut to max_chars characters. */
void tailchat_bubble(char *out, size_t n, const tailchat_bubble_t *b, size_t max_chars);

/* The reactions the device offers, and how an emoji is written in a font
 * with no emoji: "+1", "<3", ...; "*" for one it does not know. */
#define TAILCHAT_REACTIONS 4
extern const char *const tailchat_reactions[TAILCHAT_REACTIONS];
const char *tailchat_emoji_label(const char *emoji);

/* A tab's label: "Requests 2" while requests wait. */
const char *tailchat_tab_label(tailchat_tab_t tab, int waiting, char *out, size_t n);

/* What a contact answered this device's request: -1 nothing yet. */
const char *tailchat_answer_text(int8_t accepted_by_them);
