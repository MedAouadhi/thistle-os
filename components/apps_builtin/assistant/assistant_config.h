// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "assistant_request.h"

#define ASSISTANT_URL_MAX     256
#define ASSISTANT_KEY_MAX     256
#define ASSISTANT_MODEL_MAX   96
#define ASSISTANT_PROMPT_MAX  1024

/* What config/assistant.json on the SD card holds:
 * {
 *   "provider": "anthropic" | "openai",
 *   "api_url": "...",
 *   "api_key": "...",
 *   "model": "...",
 *   "system_prompt": "..."
 * }
 * A file without "provider" predates it: one whose URL ends in
 * /chat/completions is OpenAI-style, any other Anthropic. */
typedef struct {
    assistant_provider_t provider;
    char api_url[ASSISTANT_URL_MAX];
    char api_key[ASSISTANT_KEY_MAX];
    char model[ASSISTANT_MODEL_MAX];
    char system_prompt[ASSISTANT_PROMPT_MAX];
} assistant_config_t;

const char *assistant_provider_id(assistant_provider_t provider);
const char *assistant_provider_default_url(assistant_provider_t provider);
/* Empty for OpenAI-style: every server names its models differently. */
const char *assistant_provider_default_model(assistant_provider_t provider);

/* The provider's defaults, with no key. */
void assistant_config_init(assistant_config_t *config,
                           assistant_provider_t provider);

/* Read a config document over the defaults of its provider. Returns false,
 * leaving Anthropic's defaults, if json is not a JSON object. */
bool assistant_config_parse(const char *json, size_t len,
                            assistant_config_t *config);

/* The config as a JSON document, heap-allocated. */
char *assistant_config_serialize(const assistant_config_t *config,
                                 size_t *out_length);

/* Why the config cannot be used yet, as a sentence; NULL if it can. */
const char *assistant_config_problem(const assistant_config_t *config);

/* Load path into config. Returns false, leaving defaults, if the file is
 * missing, too large or malformed. */
bool assistant_config_load(const char *path, assistant_config_t *config);

/* Write config to path, creating its directory. */
bool assistant_config_save(const char *path, const assistant_config_t *config);
