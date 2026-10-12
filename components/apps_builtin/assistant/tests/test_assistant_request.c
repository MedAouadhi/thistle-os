// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors

#include "assistant_json.h"
#include "assistant_request.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HISTORY_COUNT 6U
#define MAX_MESSAGE_TEXT 511U

static void fill_hostile_maximum(char *text, size_t length)
{
    static const char pattern[] = {'A', '"', '\\', '\n', '\r', '\t', '\x01'};
    for (size_t index = 0; index < length; index++) {
        text[index] = pattern[index % sizeof(pattern)];
    }
    text[length] = '\0';
}

/* ------------------------------------------------------------------ */
/* Requests                                                             */
/* ------------------------------------------------------------------ */

static void test_maximum_history_is_safely_serialized(void)
{
    char content[MAX_MESSAGE_TEXT + 1U];
    char system_prompt[256];
    assistant_request_message_t history[HISTORY_COUNT];
    fill_hostile_maximum(content, MAX_MESSAGE_TEXT);
    fill_hostile_maximum(system_prompt, sizeof(system_prompt) - 1U);
    for (size_t index = 0; index < HISTORY_COUNT; index++) {
        history[index].role = index % 2U == 0 ? "user" : "assistant";
        history[index].content = content;
    }

    const assistant_provider_t providers[] = {
        ASSISTANT_PROVIDER_ANTHROPIC, ASSISTANT_PROVIDER_OPENAI,
    };
    for (size_t p = 0; p < 2; p++) {
        size_t length = 0;
        char *body = assistant_request_build(
            providers[p], "model-with-\"quote", system_prompt, history,
            HISTORY_COUNT, content, &length);
        assert(body != NULL);
        assert(length == strlen(body));
        assert(length > 2048U);
        assert(strstr(body, "\\\"") != NULL);
        assert(strstr(body, "\\\\") != NULL);
        assert(strstr(body, "\\u0001") != NULL);
        assert(strcmp(body + length - 3U, "}]}") == 0);

        /* What was escaped reads back as what went in. */
        char decoded[MAX_MESSAGE_TEXT + 1U];
        assert(assistant_json_get_string(body, length, "messages.0.content",
                                         decoded, sizeof(decoded), NULL));
        assert(strcmp(decoded, providers[p] == ASSISTANT_PROVIDER_OPENAI
                                   ? system_prompt : content) == 0);
        free(body);
    }
}

static void test_anthropic_request_shape(void)
{
    const assistant_request_message_t history[] = {
        { "user", "first" }, { "assistant", "answer" },
    };
    size_t length = 0;
    char *body = assistant_request_build(ASSISTANT_PROVIDER_ANTHROPIC,
                                         "claude", "be brief", history, 2,
                                         "second", &length);
    assert(body != NULL);
    assert(strcmp(body,
        "{\"model\":\"claude\",\"max_tokens\":512,\"system\":\"be brief\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"first\"},"
        "{\"role\":\"assistant\",\"content\":\"answer\"},"
        "{\"role\":\"user\",\"content\":\"second\"}]}") == 0);
    free(body);

    /* An empty system prompt is left out rather than sent empty. */
    body = assistant_request_build(ASSISTANT_PROVIDER_ANTHROPIC, "claude", "",
                                   NULL, 0, "hi", &length);
    assert(body != NULL);
    assert(strcmp(body,
        "{\"model\":\"claude\",\"max_tokens\":512,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}") == 0);
    free(body);
}

static void test_openai_request_shape(void)
{
    const assistant_request_message_t history[] = {
        { "user", "first" }, { "assistant", "answer" },
    };
    size_t length = 0;
    char *body = assistant_request_build(ASSISTANT_PROVIDER_OPENAI, "gpt",
                                         "be brief", history, 2, "second",
                                         &length);
    assert(body != NULL);
    assert(strcmp(body,
        "{\"model\":\"gpt\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"be brief\"},"
        "{\"role\":\"user\",\"content\":\"first\"},"
        "{\"role\":\"assistant\",\"content\":\"answer\"},"
        "{\"role\":\"user\",\"content\":\"second\"}]}") == 0);
    free(body);

    body = assistant_request_build(ASSISTANT_PROVIDER_OPENAI, "gpt", "", NULL,
                                   0, "hi", &length);
    assert(body != NULL);
    assert(strcmp(body, "{\"model\":\"gpt\",\"messages\":["
                        "{\"role\":\"user\",\"content\":\"hi\"}]}") == 0);
    free(body);
}

static void test_invalid_inputs_fail_closed(void)
{
    size_t length = 123U;
    assert(assistant_request_build(ASSISTANT_PROVIDER_ANTHROPIC, NULL,
                                   "system", NULL, 0, "message",
                                   &length) == NULL);
    assert(length == 0U);
    assert(assistant_request_build(ASSISTANT_PROVIDER_ANTHROPIC, "model",
                                   "system", NULL, 1, "message",
                                   &length) == NULL);
    assert(assistant_request_build(ASSISTANT_PROVIDER_ANTHROPIC, "model",
                                   "system", NULL, 0, "message",
                                   NULL) == NULL);
    assert(assistant_request_build((assistant_provider_t)7, "model",
                                   "system", NULL, 0, "message",
                                   &length) == NULL);
}

/* ------------------------------------------------------------------ */
/* Context                                                              */
/* ------------------------------------------------------------------ */

#define Q(text) { ASSISTANT_TURN_QUESTION, text }
#define R(text) { ASSISTANT_TURN_REPLY, text }
#define N(text) { ASSISTANT_TURN_NOTE, text }

static void assert_history(const assistant_request_message_t *history,
                           size_t count, const char *const *expected)
{
    for (size_t index = 0; index < count; index++) {
        assert(strcmp(history[index].role,
                      index % 2U == 0 ? "user" : "assistant") == 0);
        assert(strcmp(history[index].content, expected[index]) == 0);
    }
    assert(expected[count] == NULL);
}

static void test_history_keeps_answered_exchanges_only(void)
{
    /* A question that drew only a note, and the note itself, are left out:
     * the API must not be told the model said "No network". */
    const assistant_turn_t turns[] = {
        Q("q1"), R("a1"),
        Q("lost"), N("No network connection."),
        Q("q2"), R("a2"),
    };
    assistant_request_message_t history[6];
    size_t count = assistant_history_select(turns, 6, 3, history);
    static const char *const expected[] = { "q1", "a1", "q2", "a2", NULL };
    assert(count == 4);
    assert_history(history, count, expected);
}

static void test_history_takes_the_most_recent_exchanges(void)
{
    const assistant_turn_t turns[] = {
        Q("q1"), R("a1"), Q("q2"), R("a2"),
        Q("q3"), R("a3"), Q("q4"), R("a4"),
    };
    assistant_request_message_t history[6];
    size_t count = assistant_history_select(turns, 8, 3, history);
    static const char *const expected[] = {
        "q2", "a2", "q3", "a3", "q4", "a4", NULL,
    };
    assert(count == 6);
    assert_history(history, count, expected);
}

static void test_history_never_starts_with_a_reply(void)
{
    /* A conversation loaded from an older file can begin mid-exchange. */
    const assistant_turn_t turns[] = {
        R("orphan"), Q("q1"), R("a1"), N("note"), R("stray"),
    };
    assistant_request_message_t history[6];
    size_t count = assistant_history_select(turns, 5, 3, history);
    static const char *const expected[] = { "q1", "a1", NULL };
    assert(count == 2);
    assert_history(history, count, expected);

    assert(assistant_history_select(turns, 0, 3, history) == 0);
    assert(assistant_history_select(turns, 1, 3, history) == 0);
    assert(assistant_history_select(turns, 5, 0, history) == 0);
}

/* ------------------------------------------------------------------ */
/* Responses                                                            */
/* ------------------------------------------------------------------ */

static assistant_reply_status_t parse(assistant_provider_t provider,
                                      const char *body, char *out,
                                      size_t out_len)
{
    return assistant_response_parse(provider, body, strlen(body), out,
                                    out_len);
}

static void test_anthropic_reply_joins_text_blocks(void)
{
    static const char body[] =
        "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"text\",\"text\":\"Caf\\u00e9 \\ud83d\\ude00\"},"
        "{\"type\":\"tool_use\",\"id\":\"t\",\"name\":\"x\",\"input\":{\"text\":\"no\"}},"
        "{\"type\":\"text\",\"text\":\"\\nsays \\\"hi\\\"\\r\\n\"}],"
        "\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":3}}";
    char out[128];
    assert(parse(ASSISTANT_PROVIDER_ANTHROPIC, body, out, sizeof(out))
           == ASSISTANT_REPLY_OK);
    assert(strcmp(out, "Caf\xc3\xa9 \xf0\x9f\x98\x80\nsays \"hi\"") == 0);
}

static void test_anthropic_reply_cut_by_max_tokens_is_marked(void)
{
    static const char body[] =
        "{\"content\":[{\"type\":\"text\",\"text\":\"Once upon a\"}],"
        "\"stop_reason\":\"max_tokens\"}";
    char out[64];
    assert(parse(ASSISTANT_PROVIDER_ANTHROPIC, body, out, sizeof(out))
           == ASSISTANT_REPLY_OK);
    assert(strcmp(out, "Once upon a...") == 0);
}

static void test_openai_reply(void)
{
    static const char body[] =
        "{\"id\":\"c\",\"object\":\"chat.completion\",\"choices\":[{\"index\":0,"
        "\"message\":{\"role\":\"assistant\",\"content\":\"Hello\\tthere\","
        "\"refusal\":null},\"finish_reason\":\"stop\"}],\"usage\":{}}";
    char out[64];
    assert(parse(ASSISTANT_PROVIDER_OPENAI, body, out, sizeof(out))
           == ASSISTANT_REPLY_OK);
    assert(strcmp(out, "Hello\tthere") == 0);

    static const char cut[] =
        "{\"choices\":[{\"message\":{\"content\":\"Partial\"},"
        "\"finish_reason\":\"length\"}]}";
    assert(parse(ASSISTANT_PROVIDER_OPENAI, cut, out, sizeof(out))
           == ASSISTANT_REPLY_OK);
    assert(strcmp(out, "Partial...") == 0);

    static const char refused[] =
        "{\"choices\":[{\"message\":{\"content\":null,"
        "\"refusal\":\"I can't help with that.\"}}]}";
    assert(parse(ASSISTANT_PROVIDER_OPENAI, refused, out, sizeof(out))
           == ASSISTANT_REPLY_OK);
    assert(strcmp(out, "I can't help with that.") == 0);
}

static void test_errors_are_surfaced(void)
{
    char out[64];
    static const char anthropic[] =
        "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\","
        "\"message\":\"invalid x-api-key\"},\"request_id\":\"r\"}";
    assert(parse(ASSISTANT_PROVIDER_ANTHROPIC, anthropic, out, sizeof(out))
           == ASSISTANT_REPLY_API_ERROR);
    assert(strcmp(out, "invalid x-api-key") == 0);

    static const char openai[] =
        "{\"error\":{\"message\":\"Incorrect API key provided\","
        "\"type\":\"invalid_request_error\",\"code\":\"invalid_api_key\"}}";
    assert(parse(ASSISTANT_PROVIDER_OPENAI, openai, out, sizeof(out))
           == ASSISTANT_REPLY_API_ERROR);
    assert(strcmp(out, "Incorrect API key provided") == 0);

    static const char flat[] = "{\"error\":\"model 'x' not found\"}";
    assert(parse(ASSISTANT_PROVIDER_OPENAI, flat, out, sizeof(out))
           == ASSISTANT_REPLY_API_ERROR);
    assert(strcmp(out, "model 'x' not found") == 0);

    /* The other provider's success shape is not mistaken for a reply. */
    static const char wrong[] =
        "{\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}";
    assert(parse(ASSISTANT_PROVIDER_OPENAI, wrong, out, sizeof(out))
           == ASSISTANT_REPLY_UNREADABLE);
    assert(out[0] == '\0');
}

static void test_long_reply_is_cut_at_a_character(void)
{
    /* 40 two-byte characters into a buffer that holds 20 and a half. */
    char body[512] = "{\"choices\":[{\"message\":{\"content\":\"";
    for (int i = 0; i < 40; i++) {
        strcat(body, "\\u00e9");
    }
    strcat(body, "\"}}]}");

    char out[42];
    assert(parse(ASSISTANT_PROVIDER_OPENAI, body, out, sizeof(out))
           == ASSISTANT_REPLY_OK);
    const size_t length = strlen(out);
    assert(length < sizeof(out));
    assert(strcmp(out + length - 3, "...") == 0);
    /* Every é before the dots is whole. */
    assert((length - 3) % 2 == 0);
    for (size_t i = 0; i < length - 3; i += 2) {
        assert((unsigned char)out[i] == 0xc3 && (unsigned char)out[i + 1] == 0xa9);
    }
}

static void test_malformed_bodies_fail_closed(void)
{
    static const char *const bodies[] = {
        "",
        "not json",
        "{\"choices\":[{\"message\":{\"content\":\"unterminated",
        "{\"choices\":[{\"message\":{\"content\":\"bad \\x escape\"}}]}",
        "{\"choices\":[{\"message\":{\"content\":\"\\ud83d\\u12\"}}]}",
        "{\"choices\":[{\"message\":{\"content\":42}}]}",
        "[1,2,3]",
    };
    char out[32];
    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        assert(parse(ASSISTANT_PROVIDER_OPENAI, bodies[i], out, sizeof(out))
               == ASSISTANT_REPLY_UNREADABLE);
        assert(out[0] == '\0');
    }

    /* A lone surrogate becomes U+FFFD rather than invalid UTF-8. */
    static const char lone[] =
        "{\"choices\":[{\"message\":{\"content\":\"a\\udc00b\"}}]}";
    assert(parse(ASSISTANT_PROVIDER_OPENAI, lone, out, sizeof(out))
           == ASSISTANT_REPLY_OK);
    assert(strcmp(out, "a\xef\xbf\xbd" "b") == 0);
}

static void test_deep_nesting_does_not_recurse_without_bound(void)
{
    /* The reply follows something nested far deeper than any API sends. */
    enum { DEPTH = 100000 };
    char *body = malloc(DEPTH * 2 + 128);
    assert(body != NULL);
    char *p = body;
    p += sprintf(p, "{\"junk\":");
    for (int i = 0; i < DEPTH; i++) *p++ = '[';
    for (int i = 0; i < DEPTH; i++) *p++ = ']';
    sprintf(p, ",\"choices\":[{\"message\":{\"content\":\"x\"}}]}");

    char out[8];
    assert(assistant_response_parse(ASSISTANT_PROVIDER_OPENAI, body,
                                    strlen(body), out, sizeof(out))
           == ASSISTANT_REPLY_UNREADABLE);
    free(body);
}

static void test_body_is_read_only_within_its_length(void)
{
    /* No NUL at the end: the parser must stop at body_len. */
    static const char text[] =
        "{\"choices\":[{\"message\":{\"content\":\"abc\"}}]}";
    const size_t length = strlen(text);
    char *body = malloc(length);
    assert(body != NULL);
    memcpy(body, text, length);

    char out[16];
    assert(assistant_response_parse(ASSISTANT_PROVIDER_OPENAI, body, length,
                                    out, sizeof(out)) == ASSISTANT_REPLY_OK);
    assert(strcmp(out, "abc") == 0);
    /* Cut inside a key: unreadable, and nothing past the end is read. */
    assert(assistant_response_parse(ASSISTANT_PROVIDER_OPENAI, body, 30,
                                    out, sizeof(out))
           == ASSISTANT_REPLY_UNREADABLE);
    free(body);
}

int main(void)
{
    test_maximum_history_is_safely_serialized();
    test_anthropic_request_shape();
    test_openai_request_shape();
    test_invalid_inputs_fail_closed();
    test_history_keeps_answered_exchanges_only();
    test_history_takes_the_most_recent_exchanges();
    test_history_never_starts_with_a_reply();
    test_anthropic_reply_joins_text_blocks();
    test_anthropic_reply_cut_by_max_tokens_is_marked();
    test_openai_reply();
    test_errors_are_surfaced();
    test_long_reply_is_cut_at_a_character();
    test_malformed_bodies_fail_closed();
    test_deep_nesting_does_not_recurse_without_bound();
    test_body_is_read_only_within_its_length();
    puts("assistant request tests passed");
    return 0;
}
