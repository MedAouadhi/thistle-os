/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — To-do list, without LVGL or files
 *
 * The list the app shows and the text of the file it keeps it in, a Markdown
 * checklist anyone can read or edit in File Transfer's page:
 *
 *     - [ ] Buy milk
 *     - [x] Pay the rent
 *
 * Host-tested in apps/todo/tests/test_todo_core.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define TODO_MAX      50
/* Bytes, the NUL included. The text field takes TODO_CHARS characters, which
 * fit whatever script they are in: four bytes is UTF-8's longest. */
#define TODO_TEXT_MAX 321
#define TODO_CHARS    80
/* The longest file todo_format() writes: each line's "- [x] " and newline. */
#define TODO_FILE_MAX (TODO_MAX * (TODO_TEXT_MAX + 7) + 1)

typedef struct {
    bool done;
    char text[TODO_TEXT_MAX];
} todo_item_t;

/* The newest task first. A task ticked off stays where it is until it is
 * cleared, so a tap never moves a row out from under the finger. */
typedef struct {
    int         count;
    todo_item_t items[TODO_MAX];
} todo_list_t;

typedef enum {
    TODO_ADDED,
    TODO_EMPTY,     /* nothing but spaces */
    TODO_FULL,
} todo_add_t;

/* Puts `text` first, without the spaces around it, any line break or tab in
 * it as a space, and cut at a character to fit. */
todo_add_t todo_add(todo_list_t *list, const char *text);

/* Out of range is ignored. */
void todo_toggle(todo_list_t *list, int index);
void todo_remove(todo_list_t *list, int index);

/* Removes every task ticked off; returns how many. */
int todo_clear_done(todo_list_t *list);

int todo_open_count(const todo_list_t *list);

/* Reads a file's text into `list`, replacing what it held. Each line is a
 * task: "- [ ] ", "- [x] " (or "[X]", or "*" for "-") says whether it is
 * done, and a line without, such as one typed into the file by hand, is a task
 * not done. A blank line, or a heading ("# ..."), is no task. Past TODO_MAX
 * tasks the rest is left out. */
void todo_parse(todo_list_t *list, const char *text);

/* Writes the list as the file's text into `out`, as snprintf() does: returns
 * the length the whole text needs, writing what fits of it. */
size_t todo_format(const todo_list_t *list, char *out, size_t cap);
