// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// The to-do list: what adding, ticking and clearing do to it, and the
// Markdown checklist it is kept in.

#include "todo/todo_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            s_failures++;                                                      \
        }                                                                      \
    } while (0)

static void expect_str(const char *actual, const char *expected, int line)
{
    if (strcmp(actual, expected) == 0) return;
    fprintf(stderr, "%s:%d:\n  expected: \"%s\"\n  actual:   \"%s\"\n", __FILE__, line, expected,
            actual);
    s_failures++;
}

#define EXPECT_STR(a, e) expect_str((a), (e), __LINE__)

/* The list is 16 KB: one, off the stack, for every test. */
static todo_list_t *s_list;

static void fresh(void)
{
    memset(s_list, 0, sizeof *s_list);
}

static const char *formatted(void)
{
    static char out[TODO_FILE_MAX];
    size_t n = todo_format(s_list, out, sizeof out);
    CHECK(n < sizeof out);
    return out;
}

static void test_a_new_task_goes_first_and_is_not_done(void)
{
    fresh();
    CHECK(todo_add(s_list, "Buy milk") == TODO_ADDED);
    CHECK(todo_add(s_list, "Call Anna") == TODO_ADDED);
    CHECK(s_list->count == 2);
    EXPECT_STR(s_list->items[0].text, "Call Anna");
    EXPECT_STR(s_list->items[1].text, "Buy milk");
    CHECK(!s_list->items[0].done && !s_list->items[1].done);
    CHECK(todo_open_count(s_list) == 2);
}

static void test_a_task_is_trimmed_and_kept_to_one_line(void)
{
    fresh();
    CHECK(todo_add(s_list, "  \tWater\nthe plants\r  ") == TODO_ADDED);
    EXPECT_STR(s_list->items[0].text, "Water the plants");
}

static void test_nothing_but_spaces_is_no_task(void)
{
    fresh();
    CHECK(todo_add(s_list, "") == TODO_EMPTY);
    CHECK(todo_add(s_list, "  \n\t ") == TODO_EMPTY);
    CHECK(todo_add(s_list, NULL) == TODO_EMPTY);
    CHECK(s_list->count == 0);
}

static void test_a_full_list_takes_no_more(void)
{
    fresh();
    for (int i = 0; i < TODO_MAX; i++) CHECK(todo_add(s_list, "x") == TODO_ADDED);
    CHECK(todo_add(s_list, "one too many") == TODO_FULL);
    CHECK(s_list->count == TODO_MAX);
    /* Empty says so first: there is nothing to make room for. */
    CHECK(todo_add(s_list, " ") == TODO_EMPTY);
}

static void test_a_long_task_is_cut_where_a_character_begins(void)
{
    fresh();
    /* "é" is two bytes; one more byte than fits puts its second byte past
     * the end, so the cut has to come before it. */
    char text[TODO_TEXT_MAX + 8];
    memset(text, 'a', TODO_TEXT_MAX - 2);
    strcpy(text + TODO_TEXT_MAX - 2, "\xc3\xa9zz");
    CHECK(todo_add(s_list, text) == TODO_ADDED);
    size_t n = strlen(s_list->items[0].text);
    CHECK(n == TODO_TEXT_MAX - 2);
    CHECK(s_list->items[0].text[n - 1] == 'a');
}

static void test_the_longest_typed_task_fits_whole(void)
{
    fresh();
    /* TODO_CHARS characters of four bytes each, the longest UTF-8 has. */
    char text[TODO_CHARS * 4 + 1];
    for (int i = 0; i < TODO_CHARS; i++) memcpy(text + i * 4, "\xf0\x9f\x93\x9e", 4);
    text[TODO_CHARS * 4] = '\0';
    CHECK(todo_add(s_list, text) == TODO_ADDED);
    EXPECT_STR(s_list->items[0].text, text);
}

static void test_ticking_off_leaves_a_task_where_it_is(void)
{
    fresh();
    todo_add(s_list, "c");
    todo_add(s_list, "b");
    todo_add(s_list, "a");
    todo_toggle(s_list, 1);
    EXPECT_STR(s_list->items[1].text, "b");
    CHECK(s_list->items[1].done);
    CHECK(todo_open_count(s_list) == 2);
    todo_toggle(s_list, 1);
    CHECK(!s_list->items[1].done);
    todo_toggle(s_list, -1);
    todo_toggle(s_list, 3);
    CHECK(todo_open_count(s_list) == 3);
}

static void test_removing_closes_the_gap(void)
{
    fresh();
    todo_add(s_list, "c");
    todo_add(s_list, "b");
    todo_add(s_list, "a");
    todo_remove(s_list, 1);
    CHECK(s_list->count == 2);
    EXPECT_STR(s_list->items[0].text, "a");
    EXPECT_STR(s_list->items[1].text, "c");
    todo_remove(s_list, 5);
    CHECK(s_list->count == 2);
    todo_remove(s_list, 1);
    todo_remove(s_list, 0);
    CHECK(s_list->count == 0);
}

static void test_clearing_removes_what_is_done_and_keeps_the_order(void)
{
    fresh();
    const char *names[] = { "e", "d", "c", "b", "a" };
    for (int i = 0; i < 5; i++) todo_add(s_list, names[i]);
    todo_toggle(s_list, 0);
    todo_toggle(s_list, 2);
    todo_toggle(s_list, 4);
    CHECK(todo_clear_done(s_list) == 3);
    CHECK(s_list->count == 2);
    EXPECT_STR(s_list->items[0].text, "b");
    EXPECT_STR(s_list->items[1].text, "d");
    CHECK(todo_clear_done(s_list) == 0);
}

static void test_the_file_is_a_markdown_checklist(void)
{
    fresh();
    todo_add(s_list, "Pay the rent");
    todo_add(s_list, "Buy milk");
    todo_toggle(s_list, 1);
    EXPECT_STR(formatted(), "- [ ] Buy milk\n- [x] Pay the rent\n");

    fresh();
    EXPECT_STR(formatted(), "");
}

static void test_format_says_how_long_the_whole_text_is(void)
{
    fresh();
    todo_add(s_list, "abc");
    todo_add(s_list, "de");
    char small[8];
    size_t need = todo_format(s_list, small, sizeof small);
    CHECK(need == strlen("- [ ] de\n- [ ] abc\n"));
    EXPECT_STR(small, "- [ ] d");
    CHECK(todo_format(s_list, NULL, 0) == need);
}

static void test_reading_back_what_was_written(void)
{
    fresh();
    todo_add(s_list, "Pay the rent");
    todo_add(s_list, "قهوة");
    todo_toggle(s_list, 0);
    char text[TODO_FILE_MAX];
    todo_format(s_list, text, sizeof text);

    todo_list_t *again = calloc(1, sizeof *again);
    todo_parse(again, text);
    CHECK(again->count == 2);
    EXPECT_STR(again->items[0].text, "قهوة");
    CHECK(again->items[0].done);
    EXPECT_STR(again->items[1].text, "Pay the rent");
    CHECK(!again->items[1].done);
    free(again);
}

static void test_reading_a_file_edited_by_hand(void)
{
    todo_parse(s_list,
               "# Shopping\r\n"
               "\n"
               "- [X] Bread\r\n"
               "* [ ] Eggs\n"
               "  - Butter  \n"
               "Jam\n"
               "[x] Tea\n"
               "- [ ]\n"
               "-\n"
               "- [x]Coffee\n"
               "No newline at the end");
    CHECK(s_list->count == 7);
    if (s_list->count != 7) return;
    EXPECT_STR(s_list->items[0].text, "Bread");
    CHECK(s_list->items[0].done);
    EXPECT_STR(s_list->items[1].text, "Eggs");
    CHECK(!s_list->items[1].done);
    EXPECT_STR(s_list->items[2].text, "Butter");
    EXPECT_STR(s_list->items[3].text, "Jam");
    CHECK(!s_list->items[3].done);
    EXPECT_STR(s_list->items[4].text, "Tea");
    CHECK(s_list->items[4].done);
    /* No space after the box: the box is part of the text. */
    EXPECT_STR(s_list->items[5].text, "[x]Coffee");
    CHECK(!s_list->items[5].done);
    EXPECT_STR(s_list->items[6].text, "No newline at the end");
}

static void test_reading_replaces_the_list_and_stops_when_full(void)
{
    fresh();
    todo_add(s_list, "old");
    todo_parse(s_list, "");
    CHECK(s_list->count == 0);
    todo_parse(s_list, NULL);
    CHECK(s_list->count == 0);

    static char text[TODO_MAX * 16 + 64];
    size_t used = 0;
    for (int i = 0; i < TODO_MAX + 5; i++) {
        used += (size_t)snprintf(text + used, sizeof text - used, "- [ ] task %d\n", i);
    }
    todo_parse(s_list, text);
    CHECK(s_list->count == TODO_MAX);
    EXPECT_STR(s_list->items[TODO_MAX - 1].text, "task 49");
}

static void test_a_full_list_of_the_longest_tasks_fits_the_file_buffer(void)
{
    fresh();
    char text[TODO_TEXT_MAX];
    memset(text, 'w', sizeof text - 1);
    text[sizeof text - 1] = '\0';
    for (int i = 0; i < TODO_MAX; i++) {
        todo_add(s_list, text);
        todo_toggle(s_list, 0);
    }
    CHECK(todo_format(s_list, NULL, 0) < TODO_FILE_MAX);
}

int main(void)
{
    s_list = calloc(1, sizeof *s_list);
    if (!s_list) return 1;

    test_a_new_task_goes_first_and_is_not_done();
    test_a_task_is_trimmed_and_kept_to_one_line();
    test_nothing_but_spaces_is_no_task();
    test_a_full_list_takes_no_more();
    test_a_long_task_is_cut_where_a_character_begins();
    test_the_longest_typed_task_fits_whole();
    test_ticking_off_leaves_a_task_where_it_is();
    test_removing_closes_the_gap();
    test_clearing_removes_what_is_done_and_keeps_the_order();
    test_the_file_is_a_markdown_checklist();
    test_format_says_how_long_the_whole_text_is();
    test_reading_back_what_was_written();
    test_reading_a_file_edited_by_hand();
    test_reading_replaces_the_list_and_stops_when_full();
    test_a_full_list_of_the_longest_tasks_fits_the_file_buffer();

    free(s_list);
    if (s_failures) {
        fprintf(stderr, "test_todo_core: %d failed\n", s_failures);
        return 1;
    }
    printf("test_todo_core: all passed\n");
    return 0;
}
