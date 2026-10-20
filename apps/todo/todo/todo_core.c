/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — To-do list, without LVGL or files
 */
#include "todo/todo_core.h"

#include <stdio.h>
#include <string.h>

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Narrows [*s, *e) to what lies between its spaces. */
static void trim(const char **s, const char **e)
{
    while (*s < *e && is_space(**s)) (*s)++;
    while (*e > *s && is_space((*e)[-1])) (*e)--;
}

/* Copies [s, e), already trimmed, as one line of a task: a break or tab in it
 * is a space, and what does not fit is cut where a character begins. */
static void copy_text(char *dst, const char *s, const char *e)
{
    size_t n = (size_t)(e - s);
    if (n > TODO_TEXT_MAX - 1) {
        n = TODO_TEXT_MAX - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        while (n > 0 && is_space(s[n - 1])) n--;
    }
    for (size_t i = 0; i < n; i++) dst[i] = is_space(s[i]) ? ' ' : s[i];
    dst[n] = '\0';
}

todo_add_t todo_add(todo_list_t *list, const char *text)
{
    const char *s = text ? text : "";
    const char *e = s + strlen(s);
    trim(&s, &e);
    if (s == e) return TODO_EMPTY;
    if (list->count >= TODO_MAX) return TODO_FULL;

    memmove(&list->items[1], &list->items[0], (size_t)list->count * sizeof list->items[0]);
    list->items[0].done = false;
    copy_text(list->items[0].text, s, e);
    list->count++;
    return TODO_ADDED;
}

void todo_toggle(todo_list_t *list, int index)
{
    if (index < 0 || index >= list->count) return;
    list->items[index].done = !list->items[index].done;
}

void todo_remove(todo_list_t *list, int index)
{
    if (index < 0 || index >= list->count) return;
    memmove(&list->items[index], &list->items[index + 1],
            (size_t)(list->count - index - 1) * sizeof list->items[0]);
    list->count--;
}

int todo_clear_done(todo_list_t *list)
{
    int kept = 0;
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].done) continue;
        if (kept != i) list->items[kept] = list->items[i];
        kept++;
    }
    int cleared = list->count - kept;
    list->count = kept;
    return cleared;
}

int todo_open_count(const todo_list_t *list)
{
    int open = 0;
    for (int i = 0; i < list->count; i++) {
        if (!list->items[i].done) open++;
    }
    return open;
}

/* Whether [*s, e) begins with `prefix` and a space or its end, stepping past
 * both if it does. */
static bool take(const char **s, const char *e, const char *prefix)
{
    size_t n = strlen(prefix);
    if ((size_t)(e - *s) < n || memcmp(*s, prefix, n) != 0) return false;
    if (*s + n < e && !is_space((*s)[n])) return false;
    *s += n;
    return true;
}

static void parse_line(todo_list_t *list, const char *s, const char *e)
{
    trim(&s, &e);
    if (s == e || *s == '#') return;

    if (take(&s, e, "-") || take(&s, e, "*")) trim(&s, &e);
    bool done = false;
    if (take(&s, e, "[x]") || take(&s, e, "[X]")) {
        done = true;
    } else {
        take(&s, e, "[ ]");
    }
    trim(&s, &e);
    if (s == e) return;

    todo_item_t *item = &list->items[list->count++];
    item->done = done;
    copy_text(item->text, s, e);
}

void todo_parse(todo_list_t *list, const char *text)
{
    list->count = 0;
    const char *s = text ? text : "";
    while (*s && list->count < TODO_MAX) {
        const char *e = strchr(s, '\n');
        if (!e) e = s + strlen(s);
        parse_line(list, s, e);
        s = *e ? e + 1 : e;
    }
}

size_t todo_format(const todo_list_t *list, char *out, size_t cap)
{
    size_t need = 0;
    if (cap > 0) out[0] = '\0';
    for (int i = 0; i < list->count; i++) {
        const todo_item_t *item = &list->items[i];
        size_t at = need < cap ? need : cap;
        int w = snprintf(cap ? out + at : NULL, cap - at, "- [%c] %s\n", item->done ? 'x' : ' ',
                         item->text);
        if (w > 0) need += (size_t)w;
    }
    return need;
}
