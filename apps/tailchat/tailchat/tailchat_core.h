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
 * `max_chars` characters (the last an ellipsis when cut) and to the buffer,
 * never inside a character. */
void tailchat_one_line(char *out, size_t n, const char *text, size_t max_chars);

/* A list row: the title, and below it the detail on one line. */
void tailchat_row(char *out, size_t n, const char *title, const char *detail, size_t max_chars);

/* A conversation's row: "You: ..." or "<name>: ...". */
void tailchat_message_row(char *out, size_t n, bool mine, const char *name, const char *text,
                          size_t max_chars);

/* A tab's label: "Requests 2" while requests wait. */
const char *tailchat_tab_label(tailchat_tab_t tab, int waiting, char *out, size_t n);

/* What a contact answered this device's request: -1 nothing yet. */
const char *tailchat_answer_text(int8_t accepted_by_them);
