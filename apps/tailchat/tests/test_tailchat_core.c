/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Host tests for the Chat app's text, which has no LVGL in it.
 *
 *     apps/tailchat/tests/run_tests.sh
 */
#include "tailchat/tailchat_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failed;

#define EXPECT_STR(got, want)                                                     \
    do {                                                                          \
        if (strcmp((got), (want)) != 0) {                                         \
            fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, \
                    (got), (want));                                               \
            s_failed++;                                                           \
        }                                                                         \
    } while (0)

static void one_line_joins_lines_and_cuts_at_a_character(void)
{
    char out[32];
    tailchat_one_line(out, sizeof out, "Are you\nhome?\r\n", 40);
    EXPECT_STR(out, "Are you home?");
    tailchat_one_line(out, sizeof out, "abcdefghij", 6);
    EXPECT_STR(out, "abcde\xe2\x80\xa6");
    /* Five characters of two bytes each, cut to four and an ellipsis */
    tailchat_one_line(out, sizeof out, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", 5);
    EXPECT_STR(out, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xe2\x80\xa6");
    /* The buffer is the limit too, never cutting a character in two */
    char small[6];
    tailchat_one_line(small, sizeof small, "\xc3\xa9\xc3\xa9\xc3\xa9", 40);
    EXPECT_STR(small, "\xc3\xa9\xc3\xa9");
    tailchat_one_line(out, sizeof out, NULL, 10);
    EXPECT_STR(out, "");
}

static void a_row_is_a_title_over_a_detail(void)
{
    char out[64];
    tailchat_row(out, sizeof out, "laptop", "Hello\nthere", 30);
    EXPECT_STR(out, "laptop\nHello there");
    tailchat_row(out, sizeof out, "laptop", "", 30);
    EXPECT_STR(out, "laptop");
    tailchat_row(out, sizeof out, "a-very-long-hostname-indeed", "x", 30);
    EXPECT_STR(out, "a-very-long-hostname-indeed\nx");
}

static void tabs_count_what_waits(void)
{
    char out[24];
    EXPECT_STR(tailchat_tab_label(TAILCHAT_TAB_REQUESTS, 0, out, sizeof out), "Requests");
    EXPECT_STR(tailchat_tab_label(TAILCHAT_TAB_REQUESTS, 3, out, sizeof out), "Requests 3");
    EXPECT_STR(tailchat_tab_label(TAILCHAT_TAB_CHATS, 3, out, sizeof out), "Chats");
    EXPECT_STR(tailchat_tab_label(TAILCHAT_TAB_CONTACTS, 0, out, sizeof out), "Contacts");
}

static void what_a_contact_answered_is_said(void)
{
    EXPECT_STR(tailchat_answer_text(1), "Accepted this device");
    EXPECT_STR(tailchat_answer_text(0), "Declined this device");
    EXPECT_STR(tailchat_answer_text(-1), "");
}

static void a_message_says_who(void)
{
    char out[64];
    tailchat_message_row(out, sizeof out, true, "Ben", "On my way", 40);
    EXPECT_STR(out, "You: On my way");
    tailchat_message_row(out, sizeof out, false, "Ben", "Hi\nthere", 40);
    EXPECT_STR(out, "Ben: Hi there");
}

int main(void)
{
    one_line_joins_lines_and_cuts_at_a_character();
    a_row_is_a_title_over_a_detail();
    tabs_count_what_waits();
    what_a_contact_answered_is_said();
    a_message_says_who();
    if (s_failed) {
        fprintf(stderr, "%d failed\n", s_failed);
        return 1;
    }
    printf("tailchat_core: all tests passed\n");
    return 0;
}
