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
    EXPECT_STR(out, "abcde...");
    /* Five characters of two bytes each, cut to four and an ellipsis */
    tailchat_one_line(out, sizeof out, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", 5);
    EXPECT_STR(out, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9...");
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

static void a_bubble_shows_what_it_quotes_its_state_and_reactions(void)
{
    char out[256];
    tailchat_bubble_t b = {.mine = true, .name = "Ben", .text = "tea", .state = "waiting"};
    tailchat_bubble(out, sizeof out, &b, 40);
    EXPECT_STR(out, "You: tea\nWaiting");
    b.state = "delivered";
    b.quote = "tea or\ncoffee?";
    b.quote_mine = false;
    tailchat_bubble(out, sizeof out, &b, 40);
    EXPECT_STR(out, "> Ben: tea or coffee?\nYou: tea\nDelivered");
    b.state = "read";
    b.react_them = "\xf0\x9f\x91\x8d";
    tailchat_bubble(out, sizeof out, &b, 40);
    EXPECT_STR(out, "> Ben: tea or coffee?\nYou: tea\nRead, Ben +1");

    tailchat_bubble_t in = {.mine = false, .name = "Ben", .text = "milk?", .state = "received",
                            .quote = "tea", .quote_mine = true, .react_me = "\xe2\x9d\xa4\xef\xb8\x8f"};
    tailchat_bubble(out, sizeof out, &in, 40);
    EXPECT_STR(out, "> You: tea\nBen: milk?\nYou <3");
    in.react_me = "";
    in.quote = NULL;
    tailchat_bubble(out, sizeof out, &in, 40);
    EXPECT_STR(out, "Ben: milk?");
    /* A reply to a message no longer kept */
    in.quote = "";
    in.quoted = true;
    tailchat_bubble(out, sizeof out, &in, 40);
    EXPECT_STR(out, "> an earlier message\nBen: milk?");
}

static void a_voice_message_shows_its_length_and_whether_it_plays(void)
{
    char out[256];
    tailchat_bubble_t b = {.mine = false, .name = "Ben", .text = "", .state = "received", .voice_ms = 61500};
    tailchat_bubble(out, sizeof out, &b, 40);
    EXPECT_STR(out, "Ben: Voice message 1:01");
    b.playing = true;
    tailchat_bubble(out, sizeof out, &b, 40);
    EXPECT_STR(out, "Ben: Voice message 1:01, playing");
    tailchat_bubble_t mine = {.mine = true, .name = "Ben", .text = "", .state = "waiting", .voice_ms = 900};
    tailchat_bubble(out, sizeof out, &mine, 40);
    EXPECT_STR(out, "You: Voice message 0:01\nWaiting");

    EXPECT_STR(tailchat_duration(out, sizeof out, 0), "0:00");
    EXPECT_STR(tailchat_duration(out, sizeof out, 59999), "0:59");
    EXPECT_STR(tailchat_duration(out, sizeof out, 120000), "2:00");
}

static void an_emoji_is_written_with_what_the_font_has(void)
{
    EXPECT_STR(tailchat_emoji_label("\xf0\x9f\x91\x8d"), "+1");
    EXPECT_STR(tailchat_emoji_label("\xe2\x9d\xa4\xef\xb8\x8f"), "<3");
    EXPECT_STR(tailchat_emoji_label("\xe2\x9d\xa4"), "<3");
    EXPECT_STR(tailchat_emoji_label("\xf0\x9f\x98\x82"), "haha");
    EXPECT_STR(tailchat_emoji_label("\xf0\x9f\x8e\x89"), "yay");
    EXPECT_STR(tailchat_emoji_label("\xf0\x9f\xa6\x84"), "*");
    EXPECT_STR(tailchat_emoji_label(""), "");
    for (int i = 0; i < TAILCHAT_REACTIONS; i++) {
        if (strcmp(tailchat_emoji_label(tailchat_reactions[i]), "*") == 0) {
            fprintf(stderr, "reaction %d has no label\n", i);
            s_failed++;
        }
    }
}

#define EXPECT(cond)                                                    \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            s_failed++;                                                 \
        }                                                               \
    } while (0)

static void an_address_is_a_tailnet_name_or_address_and_a_port(void)
{
    char host[64];
    uint16_t port = 1;
    EXPECT(tailchat_parse_address("homelab", host, sizeof host, &port));
    EXPECT_STR(host, "homelab");
    EXPECT(port == 0);
    EXPECT(tailchat_parse_address("  100.101.1.2:4811 ", host, sizeof host, &port));
    EXPECT_STR(host, "100.101.1.2");
    EXPECT(port == 4811);
    EXPECT(tailchat_parse_address("laptop.tail1234.ts.net", host, sizeof host, &port));
    EXPECT_STR(host, "laptop.tail1234.ts.net");
    EXPECT(port == 0);
    EXPECT(!tailchat_parse_address("", host, sizeof host, &port));
    EXPECT(!tailchat_parse_address("   ", host, sizeof host, &port));
    EXPECT(!tailchat_parse_address("homelab:", host, sizeof host, &port));
    EXPECT(!tailchat_parse_address("homelab:0", host, sizeof host, &port));
    EXPECT(!tailchat_parse_address("homelab:70000", host, sizeof host, &port));
    EXPECT(!tailchat_parse_address("home lab", host, sizeof host, &port));
    EXPECT(!tailchat_parse_address(NULL, host, sizeof host, &port));
    char tiny[4];
    EXPECT(!tailchat_parse_address("homelab", tiny, sizeof tiny, &port));
}

static void the_check_interval_goes_round_its_choices(void)
{
    char out[48];
    EXPECT_STR(tailchat_check_label(15, out, sizeof out), "Check while asleep: 15 min");
    EXPECT_STR(tailchat_check_label(60, out, sizeof out), "Check while asleep: 1 h");
    EXPECT_STR(tailchat_check_label(0, out, sizeof out), "Check while asleep: off");
    EXPECT(tailchat_next_check_minutes(5) == 15);
    EXPECT(tailchat_next_check_minutes(15) == 30);
    EXPECT(tailchat_next_check_minutes(30) == 60);
    EXPECT(tailchat_next_check_minutes(60) == 0);
    EXPECT(tailchat_next_check_minutes(0) == 5);
    EXPECT(tailchat_next_check_minutes(20) == 30);  /* set elsewhere: the next choice above */
    EXPECT(tailchat_next_check_minutes(500) == 0);
}

int main(void)
{
    an_address_is_a_tailnet_name_or_address_and_a_port();
    the_check_interval_goes_round_its_choices();
    a_bubble_shows_what_it_quotes_its_state_and_reactions();
    a_voice_message_shows_its_length_and_whether_it_plays();
    an_emoji_is_written_with_what_the_font_has();
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
