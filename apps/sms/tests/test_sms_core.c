/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — host tests for sms_core.c
 */
#include "sms/sms_core.h"

#include <stdio.h>
#include <string.h>

static int s_failed;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            s_failed++;                                                   \
        }                                                                 \
    } while (0)

static void test_a_number_as_people_write_it(void)
{
    char out[24];
    CHECK(sms_number("+49 (160) 987-6543", out, sizeof out) && strcmp(out, "+491609876543") == 0);
    CHECK(sms_number("0176/123.456", out, sizeof out) && strcmp(out, "0176123456") == 0);
    CHECK(!sms_number("Ben", out, sizeof out) && out[0] == '\0');
    CHECK(!sms_number("+", out, sizeof out));
    CHECK(!sms_number("49+1", out, sizeof out));   /* '+' only in front */
    CHECK(!sms_number("", out, sizeof out));
    CHECK(!sms_number(NULL, out, sizeof out));
    char small[4];
    CHECK(!sms_number("12345", small, sizeof small) && small[0] == '\0');
}

static void test_numbers_match_by_their_last_nine_digits(void)
{
    CHECK(sms_numbers_match("+4917612345678", "0176 12345678"));
    CHECK(sms_numbers_match("+49 160 9876543", "+491609876543"));
    CHECK(!sms_numbers_match("+4917612345678", "+4917612345679"));
    /* Short numbers, all their digits. */
    CHECK(sms_numbers_match("22222", "22222"));
    CHECK(!sms_numbers_match("22222", "122222"));
    /* A sender's name matches only itself. */
    CHECK(sms_numbers_match("DHL", "DHL"));
    CHECK(!sms_numbers_match("DHL", "dhl"));
    CHECK(!sms_numbers_match("DHL", "+4917612345678"));
}

int main(void)
{
    test_a_number_as_people_write_it();
    test_numbers_match_by_their_last_nine_digits();
    if (s_failed) {
        fprintf(stderr, "%d failed\n", s_failed);
        return 1;
    }
    printf("sms_core: all passed\n");
    return 0;
}
