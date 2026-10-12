// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors

#pragma once

#include <stddef.h>

typedef enum {
    /* Anthropic's Messages API: x-api-key, a top-level system prompt. */
    ASSISTANT_PROVIDER_ANTHROPIC = 0,
    /* OpenAI's Chat Completions API, which most other hosted and local
     * servers also speak: a bearer token, the system prompt as a message. */
    ASSISTANT_PROVIDER_OPENAI,
} assistant_provider_t;

/* The longest reply Anthropic is asked for: about 2,000 characters, which
 * is what the chat keeps of a reply. Chat Completions is sent no limit,
 * since OpenAI's newer models reject max_tokens and other servers do not
 * know its replacement. */
#define ASSISTANT_ANTHROPIC_MAX_TOKENS 512

typedef struct {
    const char *role;
    const char *content;
} assistant_request_message_t;

/* Build one request body as an exactly sized heap allocation. An empty
 * system prompt is left out. Returns NULL on invalid input, size overflow,
 * or allocation failure. */
char *assistant_request_build(assistant_provider_t provider,
                              const char *model,
                              const char *system_prompt,
                              const assistant_request_message_t *history,
                              size_t history_count,
                              const char *user_message,
                              size_t *out_length);

typedef enum {
    ASSISTANT_TURN_QUESTION,
    ASSISTANT_TURN_REPLY,
    /* Shown in the chat but never sent: no network, an API error, ... */
    ASSISTANT_TURN_NOTE,
} assistant_turn_kind_t;

typedef struct {
    assistant_turn_kind_t kind;
    const char *text;
} assistant_turn_t;

/* Choose the context sent ahead of a new question from the conversation so
 * far, oldest first: the last max_exchanges questions that were answered,
 * each followed by its answer. Notes, and questions that got no answer, are
 * left out, so the result starts with a user turn and alternates. out must
 * hold 2 * max_exchanges entries; returns how many were written. */
size_t assistant_history_select(const assistant_turn_t *turns,
                                size_t turn_count,
                                size_t max_exchanges,
                                assistant_request_message_t *out);

typedef enum {
    ASSISTANT_REPLY_OK,         /* out holds the reply text */
    ASSISTANT_REPLY_API_ERROR,  /* out holds the API's error message */
    ASSISTANT_REPLY_UNREADABLE, /* neither was found; out is empty */
} assistant_reply_status_t;

/* Read a response body. A reply longer than out, or one the model stopped
 * for running out of tokens, ends in "...". Carriage returns are dropped. */
assistant_reply_status_t assistant_response_parse(assistant_provider_t provider,
                                                  const char *body,
                                                  size_t body_len,
                                                  char *out,
                                                  size_t out_len);
