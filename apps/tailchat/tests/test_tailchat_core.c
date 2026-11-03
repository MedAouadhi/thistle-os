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

#define EXPECT(cond)                                                    \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            s_failed++;                                                 \
        }                                                               \
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

static void an_excerpt_keeps_its_lines_and_says_when_it_is_cut(void)
{
    char out[64];
    EXPECT(!tailchat_excerpt(out, sizeof out, "tea\r\nor\tcoffee?\n\n", 100, 8));
    EXPECT_STR(out, "tea\nor coffee?");
    EXPECT(tailchat_excerpt(out, sizeof out, "abcdefghij", 6, 8));
    EXPECT_STR(out, "abcde...");
    EXPECT(!tailchat_excerpt(out, sizeof out, "abcdef", 6, 8));
    EXPECT_STR(out, "abcdef");
    EXPECT(tailchat_excerpt(out, sizeof out, "1\n2\n3\n4", 100, 3));
    EXPECT_STR(out, "1\n2\n3...");
    /* Blank lines count, but never more than one in a row */
    EXPECT(!tailchat_excerpt(out, sizeof out, "a\n\n\n\nb", 100, 3));
    EXPECT_STR(out, "a\n\nb");
    /* The buffer is the limit too, never inside a character */
    char small[8];
    EXPECT(tailchat_excerpt(small, sizeof small, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", 100, 8));
    EXPECT_STR(small, "\xc3\xa9\xc3\xa9...");
    EXPECT(!tailchat_excerpt(out, sizeof out, NULL, 10, 3));
    EXPECT_STR(out, "");
}

static void what_a_contact_answered_is_said(void)
{
    EXPECT_STR(tailchat_answer_text(1), "Accepted this device");
    EXPECT_STR(tailchat_answer_text(0), "Declined this device");
    EXPECT_STR(tailchat_answer_text(-1), "");
}

static void a_length_is_minutes_and_seconds(void)
{
    char out[16];
    EXPECT_STR(tailchat_duration(out, sizeof out, 0), "0:00");
    EXPECT_STR(tailchat_duration(out, sizeof out, 900), "0:01");
    EXPECT_STR(tailchat_duration(out, sizeof out, 59999), "0:59");
    EXPECT_STR(tailchat_duration(out, sizeof out, 61500), "1:01");
    EXPECT_STR(tailchat_duration(out, sizeof out, 120000), "2:00");
}

static void an_emoji_is_logged_by_name(void)
{
    EXPECT_STR(tailchat_emoji_label("\xf0\x9f\x91\x8d"), "+1");
    EXPECT_STR(tailchat_emoji_label("\xe2\x9d\xa4\xef\xb8\x8f"), "<3");
    EXPECT_STR(tailchat_emoji_label("\xe2\x9d\xa4"), "<3");
    EXPECT_STR(tailchat_emoji_label("\xf0\x9f\x98\x82"), "haha");
    EXPECT_STR(tailchat_emoji_label("\xf0\x9f\x98\xae"), "wow");
    EXPECT_STR(tailchat_emoji_label("\xf0\x9f\xa6\x84"), "*");
    EXPECT_STR(tailchat_emoji_label(""), "");
    for (int i = 0; i < TAILCHAT_REACTIONS; i++) {
        if (strcmp(tailchat_emoji_label(tailchat_reactions[i]), "*") == 0) {
            fprintf(stderr, "reaction %d has no label\n", i);
            s_failed++;
        }
    }
}

static void a_monogram_is_the_first_letters_of_two_words(void)
{
    char out[16];
    EXPECT_STR(tailchat_initials("Lena", out, sizeof out), "L");
    EXPECT_STR(tailchat_initials("ben smith", out, sizeof out), "BS");
    EXPECT_STR(tailchat_initials("  Ada  Lovelace King", out, sizeof out), "AL");
    EXPECT_STR(tailchat_initials("lena-laptop", out, sizeof out), "L");
    EXPECT_STR(tailchat_initials("100.64.0.2", out, sizeof out), "1");
    /* A letter of two bytes is kept whole, and not made a capital */
    EXPECT_STR(tailchat_initials("\xc3\xa9lise", out, sizeof out), "\xc3\xa9");
    EXPECT_STR(tailchat_initials("(Mo)", out, sizeof out), "M");
    EXPECT_STR(tailchat_initials("", out, sizeof out), "?");
    EXPECT_STR(tailchat_initials(NULL, out, sizeof out), "?");
    EXPECT_STR(tailchat_initials("--", out, sizeof out), "?");
}

static struct tm day_at(int year, int mon, int mday, int hour, int min)
{
    /* What localtime() would say: the weekday and day of the year too. */
    struct tm t = {.tm_year = year - 1900, .tm_mon = mon - 1, .tm_mday = mday, .tm_hour = hour, .tm_min = min,
                   .tm_isdst = 0};
    time_t s = timegm(&t);
    gmtime_r(&s, &t);
    return t;
}

static void a_conversation_says_when_in_as_few_words_as_will_do(void)
{
    char out[32];
    struct tm now = day_at(2026, 11, 4, 9, 30);   /* a Wednesday */
    struct tm t = day_at(2026, 11, 4, 8, 5);
    EXPECT_STR(tailchat_when(&now, &t, true, out, sizeof out), "08:05");
    EXPECT_STR(tailchat_when(&now, &t, false, out, sizeof out), "8:05 AM");
    t = day_at(2026, 11, 4, 0, 0);
    EXPECT_STR(tailchat_when(&now, &t, false, out, sizeof out), "12:00 AM");
    t = day_at(2026, 11, 3, 23, 59);
    EXPECT_STR(tailchat_when(&now, &t, true, out, sizeof out), "Yesterday");
    t = day_at(2026, 11, 2, 12, 0);
    EXPECT_STR(tailchat_when(&now, &t, true, out, sizeof out), "Mon");
    t = day_at(2026, 10, 29, 12, 0);   /* six days before: still the week */
    EXPECT_STR(tailchat_when(&now, &t, true, out, sizeof out), "Thu");
    t = day_at(2026, 10, 28, 12, 0);
    EXPECT_STR(tailchat_when(&now, &t, true, out, sizeof out), "28 Oct");
    t = day_at(2025, 12, 31, 12, 0);
    EXPECT_STR(tailchat_when(&now, &t, true, out, sizeof out), "31 Dec 2025");
    /* The writer's clock ahead of this one's */
    t = day_at(2026, 11, 4, 9, 40);
    EXPECT_STR(tailchat_when(&now, &t, true, out, sizeof out), "09:40");
    t = day_at(2026, 11, 4, 13, 7);
    EXPECT_STR(tailchat_clock(&t, true, out, sizeof out), "13:07");
    EXPECT_STR(tailchat_clock(&t, false, out, sizeof out), "1:07 PM");
    /* Across a year's end */
    now = day_at(2027, 1, 1, 10, 0);
    t = day_at(2026, 12, 31, 22, 0);
    EXPECT_STR(tailchat_when(&now, &t, true, out, sizeof out), "Yesterday");
}

static void a_day_heading_names_the_day(void)
{
    char out[40];
    struct tm now = day_at(2026, 11, 4, 9, 30);
    struct tm t = day_at(2026, 11, 4, 1, 0);
    EXPECT_STR(tailchat_day(&now, &t, out, sizeof out), "Today");
    t = day_at(2026, 11, 3, 1, 0);
    EXPECT_STR(tailchat_day(&now, &t, out, sizeof out), "Yesterday");
    t = day_at(2026, 11, 1, 1, 0);
    EXPECT_STR(tailchat_day(&now, &t, out, sizeof out), "Sunday");
    t = day_at(2026, 10, 2, 1, 0);
    EXPECT_STR(tailchat_day(&now, &t, out, sizeof out), "Friday, 2 October");
    t = day_at(2025, 11, 4, 1, 0);
    EXPECT_STR(tailchat_day(&now, &t, out, sizeof out), "4 November 2025");
    struct tm a = day_at(2026, 11, 4, 0, 0), b = day_at(2026, 11, 4, 23, 59), c = day_at(2025, 11, 4, 12, 0);
    EXPECT(tailchat_same_day(&a, &b));
    EXPECT(!tailchat_same_day(&a, &c));
}

static void messages_close_together_from_one_writer_make_a_run(void)
{
    uint64_t t = 1793541600000ULL;
    EXPECT(tailchat_joins(true, t, true, t + 60000));
    EXPECT(tailchat_joins(false, t, false, t + TAILCHAT_RUN_MS));
    EXPECT(!tailchat_joins(false, t, false, t + TAILCHAT_RUN_MS + 1));
    EXPECT(!tailchat_joins(true, t, false, t + 1000));
    /* A clock that disagrees puts a later message first: still a run */
    EXPECT(tailchat_joins(true, t, true, t - 2000));
    EXPECT(!tailchat_joins(true, t, true, t - TAILCHAT_RUN_MS - 1));
}

static void a_contact_says_what_it_is_doing(void)
{
    tailchat_presence_t p = {.seen_name = "lena-laptop"};
    EXPECT_STR(tailchat_presence(&p), "lena-laptop");
    p.connected = true;
    EXPECT_STR(tailchat_presence(&p), "Online");
    p.waiting = true;
    EXPECT_STR(tailchat_presence(&p), "Sending...");
    p.connected = false;
    EXPECT_STR(tailchat_presence(&p), "Waiting to send");
    p.pending = true;
    EXPECT_STR(tailchat_presence(&p), "Not reached yet");
    p.typing = true;
    EXPECT_STR(tailchat_presence(&p), "typing...");
    tailchat_presence_t none = {0};
    EXPECT_STR(tailchat_presence(&none), "");
}

static void a_waveform_is_the_same_for_a_message_each_time(void)
{
    uint8_t a[24], b[24], c[24];
    tailchat_waveform("0123456789abcdef0123456789abcdef", a, sizeof a, 9);
    tailchat_waveform("0123456789abcdef0123456789abcdef", b, sizeof b, 9);
    tailchat_waveform("ffffffffffffffffffffffffffffffff", c, sizeof c, 9);
    EXPECT(memcmp(a, b, sizeof a) == 0);
    EXPECT(memcmp(a, c, sizeof a) != 0);
    bool varied = false;
    for (size_t i = 0; i < sizeof a; i++) {
        EXPECT(a[i] >= 1 && a[i] <= 9);
        if (i && a[i] != a[i - 1]) varied = true;
    }
    EXPECT(varied);
    tailchat_waveform(NULL, a, sizeof a, 9);
    for (size_t i = 0; i < sizeof a; i++) EXPECT(a[i] >= 1 && a[i] <= 9);
}

static void a_message_sent_ticks_as_it_goes(void)
{
    EXPECT(tailchat_tick(true, "waiting") == TAILCHAT_TICK_WAITING);
    EXPECT(tailchat_tick(true, "delivered") == TAILCHAT_TICK_DELIVERED);
    EXPECT(tailchat_tick(true, "read") == TAILCHAT_TICK_READ);
    EXPECT(tailchat_tick(false, "read") == TAILCHAT_TICK_NONE);
    EXPECT(tailchat_tick(true, "received") == TAILCHAT_TICK_NONE);
    EXPECT(tailchat_tick(true, NULL) == TAILCHAT_TICK_NONE);
}


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
    EXPECT_STR(tailchat_check_label(15, out, sizeof out), "Every 15 min");
    EXPECT_STR(tailchat_check_label(60, out, sizeof out), "Every hour");
    EXPECT_STR(tailchat_check_label(120, out, sizeof out), "Every 2 hours");
    EXPECT_STR(tailchat_check_label(0, out, sizeof out), "Off");
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
    one_line_joins_lines_and_cuts_at_a_character();
    what_a_contact_answered_is_said();
    an_excerpt_keeps_its_lines_and_says_when_it_is_cut();
    a_length_is_minutes_and_seconds();
    an_emoji_is_logged_by_name();
    a_monogram_is_the_first_letters_of_two_words();
    a_conversation_says_when_in_as_few_words_as_will_do();
    a_day_heading_names_the_day();
    messages_close_together_from_one_writer_make_a_run();
    a_contact_says_what_it_is_doing();
    a_waveform_is_the_same_for_a_message_each_time();
    a_message_sent_ticks_as_it_goes();
    if (s_failed) {
        fprintf(stderr, "%d failed\n", s_failed);
        return 1;
    }
    printf("tailchat_core: all tests passed\n");
    return 0;
}
