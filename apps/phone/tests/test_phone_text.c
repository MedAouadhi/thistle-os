// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// The Phone app's words for when a call was, the dialler's keys, initials
// and numbers compared. Run in UTC (run_tests.sh sets TZ).

#include "phone/phone_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int s_failures;

static void expect_str(const char *what, const char *actual, const char *expected, int line)
{
    if (strcmp(actual, expected) == 0) return;
    fprintf(stderr, "%s:%d: %s\n  expected: \"%s\"\n  actual:   \"%s\"\n", __FILE__, line, what,
            expected, actual);
    s_failures++;
}

static void expect_true(const char *what, int ok, int line)
{
    if (ok) return;
    fprintf(stderr, "%s:%d: %s\n", __FILE__, line, what);
    s_failures++;
}

#define EXPECT_STR(what, actual, expected) expect_str(what, actual, expected, __LINE__)
#define EXPECT(what, ok) expect_true(what, ok, __LINE__)

/* 2026-10-14 15:30:00 UTC, a Wednesday. */
#define NOW ((time_t)1791991800)
#define HOUR 3600
#define DAY (24 * HOUR)

static void when_is(uint32_t when, int h24, const char *expected, int line)
{
    char out[24];
    phone_text_when(when, NOW, h24, out, sizeof out);
    expect_str("when", out, expected, line);
}

static void test_when(void)
{
    when_is(0, 1, "", __LINE__);
    when_is((uint32_t)(NOW - 85 * 60), 1, "14:05", __LINE__);
    when_is((uint32_t)(NOW - 85 * 60), 0, "2:05 PM", __LINE__);
    when_is((uint32_t)(NOW - 15 * HOUR - 30 * 60), 1, "00:00", __LINE__);  /* midnight is today */
    when_is((uint32_t)(NOW - 15 * HOUR - 30 * 60), 0, "12:00 AM", __LINE__);
    when_is((uint32_t)(NOW - 15 * HOUR - 30 * 60 - 1), 1, "Yesterday", __LINE__);
    when_is((uint32_t)(NOW - DAY), 1, "Yesterday", __LINE__);
    when_is((uint32_t)(NOW - 2 * DAY), 1, "Mon", __LINE__);
    when_is((uint32_t)(NOW - 6 * DAY), 1, "Thu", __LINE__);
    when_is((uint32_t)(NOW - 7 * DAY), 1, "7 Oct", __LINE__);
    when_is((uint32_t)(NOW - 300 * DAY), 1, "Dec 2025", __LINE__);
    /* A clock set wrong: dated, not "today". */
    when_is((uint32_t)(NOW + HOUR), 1, "14 Oct", __LINE__);
}

static void test_dial(void)
{
    EXPECT("digits", phone_text_dial_char('7') == '7');
    EXPECT("plus", phone_text_dial_char('+') == '+');
    EXPECT("star", phone_text_dial_char('*') == '*');
    EXPECT("hash", phone_text_dial_char('#') == '#');
    EXPECT("letter", phone_text_dial_char('a') == 0);
    EXPECT("space", phone_text_dial_char(' ') == 0);
    EXPECT("a key code", phone_text_dial_char(0x1000 + '5') == 0);

    char number[5] = "";
    EXPECT("append", phone_text_dial_append(number, sizeof number, '0'));
    EXPECT("append", phone_text_dial_append(number, sizeof number, '1'));
    EXPECT("append", phone_text_dial_append(number, sizeof number, '7'));
    EXPECT("append", phone_text_dial_append(number, sizeof number, '6'));
    EXPECT("full", !phone_text_dial_append(number, sizeof number, '9'));
    EXPECT_STR("appended", number, "0176");
    EXPECT("delete", phone_text_dial_delete(number));
    EXPECT_STR("deleted", number, "017");
    number[0] = '\0';
    EXPECT("delete empty", !phone_text_dial_delete(number));
}

static void initials_are(const char *name, const char *expected, int line)
{
    char out[12];
    phone_text_initials(name, out, sizeof out);
    expect_str(name, out, expected, line);
}

static void test_initials(void)
{
    initials_are("Anna Schmidt", "AS", __LINE__);
    initials_are("anna maria schmidt", "AS", __LINE__);
    initials_are("J\xC3\xBCrgen", "J", __LINE__);
    initials_are("\xC3\x9C" "ber Mann", "\xC3\x9C" "M", __LINE__);
    initials_are("\xD9\x85\xD8\xAD\xD9\x85\xD8\xAF", "\xD9\x85", __LINE__);
    initials_are("  Dr. (Who)  ", "DW", __LINE__);
    initials_are("+49 151 23456789", "", __LINE__);
    initials_are("Room 101 Desk", "RD", __LINE__);
    initials_are("", "", __LINE__);
    char tiny[2];
    phone_text_initials("Anna Schmidt", tiny, sizeof tiny);
    EXPECT_STR("cut to fit", tiny, "A");
}

static void test_numbers(void)
{
    EXPECT("ascii", phone_text_ascii("Anna Schmidt"));
    EXPECT("not ascii", !phone_text_ascii("J\xC3\xBCrgen"));
    EXPECT("same", phone_text_same_number("+49 151 23456789", "+4915123456789"));
    EXPECT("same", phone_text_same_number("(0176) 123-45", "017612345"));
    EXPECT("different", !phone_text_same_number("+4915123456789", "015123456789"));
    EXPECT("different length", !phone_text_same_number("0176", "01761"));
    EXPECT("empty", phone_text_same_number("", " "));

    char out[16];
    phone_text_compact("+49 (151) 234-56.78", out, sizeof out);
    EXPECT_STR("compact", out, "+491512345678");
    phone_text_compact("*31# 0176", out, sizeof out);
    EXPECT_STR("codes", out, "*31#0176");
    char tiny[4];
    phone_text_compact("0176 1234", tiny, sizeof tiny);
    EXPECT_STR("cut to fit", tiny, "017");
}

int main(void)
{
    test_when();
    test_dial();
    test_initials();
    test_numbers();
    if (s_failures) {
        fprintf(stderr, "%d phone_text checks failed\n", s_failures);
        return 1;
    }
    printf("phone_text: all checks passed\n");
    return 0;
}
