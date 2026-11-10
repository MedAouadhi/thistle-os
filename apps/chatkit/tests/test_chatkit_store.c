/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — host tests for chatkit_store.c
 */
#include "chatkit/chatkit_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failed;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            s_failed++;                                                   \
        }                                                                 \
    } while (0)

static void test_a_conversation_keeps_its_latest_messages_oldest_first(void)
{
    chatkit_store_t st = {0};
    chatkit_conv_t *c = chatkit_ensure(&st, "+491701", "Ben", false);
    char text[16];
    for (int i = 0; i < CHATKIT_HISTORY + 5; i++) {
        snprintf(text, sizeof text, "m%d", i);
        chatkit_add(&st, c, false, "Ben", text, 0, 0, CHATKIT_NONE);
    }
    CHECK(chatkit_shown(c) == CHATKIT_HISTORY);
    CHECK(strcmp(chatkit_msg(c, 0)->text, "m5") == 0);
    CHECK(strcmp(chatkit_msg(c, CHATKIT_HISTORY - 1)->text, "m24") == 0);
    CHECK(chatkit_msg(c, CHATKIT_HISTORY) == NULL);
    /* No more unread than it keeps. */
    CHECK(c->unread == CHATKIT_HISTORY);
    chatkit_clear(&st);
}

static void test_what_arrives_on_screen_is_read(void)
{
    chatkit_store_t st = {0};
    chatkit_conv_t *a = chatkit_ensure(&st, "a", "A", false);
    chatkit_conv_t *b = chatkit_ensure(&st, "b", "B", false);
    chatkit_add(&st, a, false, "A", "one", 0, 0, CHATKIT_NONE);
    CHECK(a->unread == 1);
    chatkit_set_viewing(&st, "a");
    CHECK(a->unread == 0);
    chatkit_add(&st, a, false, "A", "two", 0, 0, CHATKIT_NONE);
    chatkit_add(&st, b, false, "B", "three", 0, 0, CHATKIT_NONE);
    CHECK(a->unread == 0);
    CHECK(b->unread == 1);
    /* The user's own are never unread. */
    chatkit_add(&st, b, true, NULL, "mine", 0, 0, CHATKIT_SENT);
    CHECK(chatkit_unread(&st) == 1);
    chatkit_set_viewing(&st, NULL);
    chatkit_add(&st, a, false, "A", "four", 0, 0, CHATKIT_NONE);
    CHECK(chatkit_unread(&st) == 2);
    chatkit_clear(&st);
}

static void test_a_full_store_forgets_the_longest_quiet(void)
{
    chatkit_store_t st = {0};
    char conv[8];
    chatkit_ensure(&st, "", "Public", true);
    for (int i = 1; i < CHATKIT_CONVS; i++) {
        snprintf(conv, sizeof conv, "c%d", i);
        chatkit_conv_t *c = chatkit_ensure(&st, conv, conv, false);
        chatkit_add(&st, c, true, NULL, "hi", 0, 0, CHATKIT_SENT);
    }
    /* c1 is the quietest but unread; c2 the quietest read. */
    chatkit_add(&st, chatkit_find(&st, "c1"), false, "x", "news", 0, 0, CHATKIT_NONE);
    chatkit_mark_read(&st, chatkit_find(&st, "c1"));
    chatkit_add(&st, chatkit_find(&st, "c3"), false, "x", "news", 0, 0, CHATKIT_NONE);
    chatkit_set_viewing(&st, "c2");
    CHECK(st.n == CHATKIT_CONVS);

    /* The Public channel is sticky and c2 on screen: c4 goes. */
    CHECK(chatkit_ensure(&st, "new", "New", false) != NULL);
    CHECK(st.n == CHATKIT_CONVS);
    CHECK(chatkit_find(&st, "c4") == NULL);
    CHECK(chatkit_find(&st, "") != NULL);
    CHECK(chatkit_find(&st, "c2") != NULL);
    CHECK(chatkit_find(&st, "c3") != NULL);
    chatkit_clear(&st);
}

static void test_news_of_a_message_sent_reaches_it(void)
{
    chatkit_store_t st = {0};
    chatkit_conv_t *c = chatkit_ensure(&st, "k", "K", false);
    chatkit_add(&st, c, true, NULL, "out", 0, 42, CHATKIT_PENDING);
    uint32_t gen = st.generation;
    CHECK(chatkit_set_status(&st, 42, CHATKIT_DELIVERED));
    CHECK(chatkit_msg(c, 0)->status == CHATKIT_DELIVERED);
    CHECK(st.generation != gen);
    CHECK(!chatkit_set_status(&st, 7, CHATKIT_FAILED));
    CHECK(!chatkit_set_status(&st, 0, CHATKIT_FAILED));
    chatkit_clear(&st);
}

static void test_conversations_are_listed_latest_first(void)
{
    chatkit_store_t st = {0};
    chatkit_conv_t *a = chatkit_ensure(&st, "a", "A", false);
    chatkit_conv_t *b = chatkit_ensure(&st, "b", "B", false);
    chatkit_conv_t *c = chatkit_ensure(&st, "c", "C", false);
    chatkit_add(&st, a, false, "A", "x", 0, 0, CHATKIT_NONE);
    chatkit_conv_t *out[CHATKIT_CONVS];
    CHECK(chatkit_order(&st, out, CHATKIT_CONVS) == 3);
    CHECK(out[0] == a && out[1] == c && out[2] == b);
    chatkit_clear(&st);
}

static void test_a_new_name_reaches_its_messages(void)
{
    chatkit_store_t st = {0};
    chatkit_conv_t *c = chatkit_ensure(&st, "+491609876543", "+491609876543", false);
    chatkit_add(&st, c, false, "+491609876543", "hallo", 0, 0, CHATKIT_NONE);
    chatkit_add(&st, c, true, NULL, "hi", 0, 0, CHATKIT_SENT);
    chatkit_retitle(&st, c, "Ben Müller");
    CHECK(strcmp(c->title, "Ben Müller") == 0);
    CHECK(strcmp(chatkit_msg(c, 0)->who, "Ben Müller") == 0);
    CHECK(strcmp(chatkit_msg(c, 1)->who, "") == 0);
    /* An empty title names nothing. */
    chatkit_ensure(&st, "+491609876543", "", false);
    CHECK(strcmp(c->title, "Ben Müller") == 0);
    chatkit_clear(&st);
}

static void test_text_is_cut_at_a_character(void)
{
    char out[6];
    chatkit_copy(out, sizeof out, "abcü");   /* ü is two bytes: 5 in all */
    CHECK(strcmp(out, "abcü") == 0);
    chatkit_copy(out, sizeof out, "abcdü");  /* 6 bytes: ü does not fit */
    CHECK(strcmp(out, "abcd") == 0);
    chatkit_copy(out, sizeof out, NULL);
    CHECK(out[0] == '\0');
}

int main(void)
{
    test_a_conversation_keeps_its_latest_messages_oldest_first();
    test_what_arrives_on_screen_is_read();
    test_a_full_store_forgets_the_longest_quiet();
    test_news_of_a_message_sent_reaches_it();
    test_conversations_are_listed_latest_first();
    test_a_new_name_reaches_its_messages();
    test_text_is_cut_at_a_character();
    if (s_failed) {
        fprintf(stderr, "%d failed\n", s_failed);
        return 1;
    }
    printf("chatkit_store: all passed\n");
    return 0;
}
