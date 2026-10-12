// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors

#include "assistant_request.h"
#include "assistant_json.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

/* ------------------------------------------------------------------ */
/* Request bodies                                                       */
/* ------------------------------------------------------------------ */

static bool write_message(assistant_json_writer_t *writer, bool comma,
                          const char *role, const char *content)
{
    return role && content
        && (!comma || assistant_json_write_raw(writer, ","))
        && assistant_json_write_raw(writer, "{\"role\":")
        && assistant_json_write_string(writer, role)
        && assistant_json_write_raw(writer, ",\"content\":")
        && assistant_json_write_string(writer, content)
        && assistant_json_write_raw(writer, "}");
}

static bool write_request(assistant_json_writer_t *writer,
                          assistant_provider_t provider,
                          const char *model,
                          const char *system_prompt,
                          const assistant_request_message_t *history,
                          size_t history_count,
                          const char *user_message)
{
    if (!model || !system_prompt || !user_message
        || (history_count > 0 && !history)) {
        return false;
    }
    const bool has_system = system_prompt[0] != '\0';

    if (!assistant_json_write_raw(writer, "{\"model\":")
        || !assistant_json_write_string(writer, model)) {
        return false;
    }

    bool first = true;
    if (provider == ASSISTANT_PROVIDER_ANTHROPIC) {
        if (!assistant_json_write_raw(writer, ",\"max_tokens\":"
                STRINGIFY(ASSISTANT_ANTHROPIC_MAX_TOKENS))
            || (has_system
                && (!assistant_json_write_raw(writer, ",\"system\":")
                    || !assistant_json_write_string(writer, system_prompt)))
            || !assistant_json_write_raw(writer, ",\"messages\":[")) {
            return false;
        }
    } else if (provider == ASSISTANT_PROVIDER_OPENAI) {
        if (!assistant_json_write_raw(writer, ",\"messages\":[")) {
            return false;
        }
        if (has_system) {
            if (!write_message(writer, false, "system", system_prompt)) {
                return false;
            }
            first = false;
        }
    } else {
        return false;
    }

    for (size_t index = 0; index < history_count; index++) {
        if (!write_message(writer, !first, history[index].role,
                           history[index].content)) {
            return false;
        }
        first = false;
    }

    return write_message(writer, !first, "user", user_message)
        && assistant_json_write_raw(writer, "]}")
        && writer->valid;
}

char *assistant_request_build(assistant_provider_t provider,
                              const char *model,
                              const char *system_prompt,
                              const assistant_request_message_t *history,
                              size_t history_count,
                              const char *user_message,
                              size_t *out_length)
{
    if (!out_length) {
        return NULL;
    }
    *out_length = 0;

    assistant_json_writer_t measure = {
        .buffer = NULL,
        .capacity = 0,
        .length = 0,
        .valid = true,
    };
    if (!write_request(&measure, provider, model, system_prompt, history,
                       history_count, user_message)
        || measure.length == SIZE_MAX) {
        return NULL;
    }

    char *body = malloc(measure.length + 1U);
    if (!body) {
        return NULL;
    }
    assistant_json_writer_t writer = {
        .buffer = body,
        .capacity = measure.length + 1U,
        .length = 0,
        .valid = true,
    };
    if (!write_request(&writer, provider, model, system_prompt, history,
                       history_count, user_message)
        || writer.length != measure.length) {
        free(body);
        return NULL;
    }
    body[writer.length] = '\0';
    *out_length = writer.length;
    return body;
}

/* ------------------------------------------------------------------ */
/* Context                                                              */
/* ------------------------------------------------------------------ */

static bool is_exchange(const assistant_turn_t *turns, size_t question)
{
    return turns[question].kind == ASSISTANT_TURN_QUESTION
        && turns[question + 1].kind == ASSISTANT_TURN_REPLY
        && turns[question].text && turns[question + 1].text;
}

size_t assistant_history_select(const assistant_turn_t *turns,
                                size_t turn_count,
                                size_t max_exchanges,
                                assistant_request_message_t *out)
{
    if (!turns || !out || max_exchanges == 0) {
        return 0;
    }

    /* Walk back to the oldest exchange that makes the cut... */
    size_t exchanges = 0;
    size_t first = turn_count;
    for (size_t end = turn_count; end >= 2 && exchanges < max_exchanges;) {
        if (is_exchange(turns, end - 2)) {
            exchanges++;
            end -= 2;
            first = end;
        } else {
            end--;
        }
    }

    /* ...then copy the exchanges from there forward, in order. */
    size_t written = 0;
    for (size_t index = first; index + 1 < turn_count
         && written < 2 * exchanges;) {
        if (is_exchange(turns, index)) {
            out[written].role = "user";
            out[written].content = turns[index].text;
            out[written + 1].role = "assistant";
            out[written + 1].content = turns[index + 1].text;
            written += 2;
            index += 2;
        } else {
            index++;
        }
    }
    return written;
}

/* ------------------------------------------------------------------ */
/* Responses                                                            */
/* ------------------------------------------------------------------ */

static bool is_space(char c)
{
    return c == ' ' || c == '\n' || c == '\t';
}

/* Drop carriage returns and surrounding blank space; mark a cut reply. */
static void finish_text(char *out, size_t out_len, bool cut)
{
    size_t length = 0;
    for (size_t read = 0; out[read] != '\0'; read++) {
        if (out[read] != '\r' && (length > 0 || !is_space(out[read]))) {
            out[length++] = out[read];
        }
    }
    while (length > 0 && is_space(out[length - 1])) {
        length--;
    }
    out[length] = '\0';

    static const char more[] = "...";
    if (!cut || out_len < sizeof(more)) {
        return;
    }
    if (length > out_len - sizeof(more)) {
        length = out_len - sizeof(more);
        /* Cut before a character, never inside one. */
        while (length > 0 && ((unsigned char)out[length] & 0xC0U) == 0x80U) {
            length--;
        }
    }
    memcpy(out + length, more, sizeof(more));
}

/* Every text block of an Anthropic reply, in order. */
static bool read_anthropic(const char *body, size_t len,
                           char *out, size_t out_len, bool *cut)
{
    size_t used = 0;
    bool found = false;
    for (unsigned index = 0; index < 32U; index++) {
        char path[32];
        snprintf(path, sizeof(path), "content.%u", index);
        if (!assistant_json_find(body, len, path)) {
            break;
        }
        snprintf(path, sizeof(path), "content.%u.type", index);
        if (!assistant_json_string_equals(body, len, path, "text")) {
            continue;
        }
        snprintf(path, sizeof(path), "content.%u.text", index);
        bool truncated = false;
        if (assistant_json_get_string(body, len, path, out + used,
                                      out_len - used, &truncated)) {
            found = true;
            used += strlen(out + used);
            if (truncated) {
                *cut = true;
                break;
            }
        }
    }
    out[used] = '\0';
    if (found
        && assistant_json_string_equals(body, len, "stop_reason",
                                        "max_tokens")) {
        *cut = true;
    }
    return found;
}

static bool read_openai(const char *body, size_t len,
                        char *out, size_t out_len, bool *cut)
{
    /* content is null when the model refused; the refusal says why. */
    if (!assistant_json_get_string(body, len, "choices.0.message.content",
                                   out, out_len, cut)
        && !assistant_json_get_string(body, len, "choices.0.message.refusal",
                                      out, out_len, cut)) {
        return false;
    }
    if (assistant_json_string_equals(body, len, "choices.0.finish_reason",
                                     "length")) {
        *cut = true;
    }
    return true;
}

/* Anthropic and OpenAI both send {"error":{"message":...}}; some servers
 * that copy OpenAI send the message as "error" itself, or at the top. */
static bool read_error(const char *body, size_t len,
                       char *out, size_t out_len, bool *cut)
{
    return assistant_json_get_string(body, len, "error.message",
                                     out, out_len, cut)
        || assistant_json_get_string(body, len, "error", out, out_len, cut)
        || assistant_json_get_string(body, len, "message", out, out_len, cut);
}

assistant_reply_status_t assistant_response_parse(assistant_provider_t provider,
                                                  const char *body,
                                                  size_t body_len,
                                                  char *out,
                                                  size_t out_len)
{
    if (!out || out_len == 0) {
        return ASSISTANT_REPLY_UNREADABLE;
    }
    out[0] = '\0';
    if (!body) {
        return ASSISTANT_REPLY_UNREADABLE;
    }

    bool cut = false;
    bool found = false;
    if (provider == ASSISTANT_PROVIDER_ANTHROPIC) {
        found = read_anthropic(body, body_len, out, out_len, &cut);
    } else if (provider == ASSISTANT_PROVIDER_OPENAI) {
        found = read_openai(body, body_len, out, out_len, &cut);
    }
    if (found) {
        finish_text(out, out_len, cut);
        return ASSISTANT_REPLY_OK;
    }

    cut = false;
    if (read_error(body, body_len, out, out_len, &cut)) {
        finish_text(out, out_len, cut);
        return ASSISTANT_REPLY_API_ERROR;
    }
    out[0] = '\0';
    return ASSISTANT_REPLY_UNREADABLE;
}
