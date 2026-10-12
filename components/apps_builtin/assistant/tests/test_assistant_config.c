// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors

#define _DEFAULT_SOURCE /* mkdtemp */

#include "assistant_config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool parse(const char *json, assistant_config_t *config)
{
    return assistant_config_parse(json, strlen(json), config);
}

static void test_defaults_per_provider(void)
{
    assistant_config_t config;
    assistant_config_init(&config, ASSISTANT_PROVIDER_ANTHROPIC);
    assert(strcmp(config.api_url, "https://api.anthropic.com/v1/messages") == 0);
    assert(config.model[0] != '\0');
    assert(config.api_key[0] == '\0');
    assert(config.system_prompt[0] != '\0');
    assert(strcmp(assistant_config_problem(&config), "Add an API key.") == 0);

    assistant_config_init(&config, ASSISTANT_PROVIDER_OPENAI);
    assert(strcmp(config.api_url,
                  "https://api.openai.com/v1/chat/completions") == 0);
    assert(config.model[0] == '\0');
    assert(strcmp(assistant_config_problem(&config), "Add a model name.") == 0);
}

static void test_file_without_provider_is_inferred_from_its_url(void)
{
    /* Every file written before the provider field existed is Anthropic,
     * unless someone pointed it at a Chat Completions server. */
    assistant_config_t config;
    assert(parse("{\"api_url\":\"https://api.anthropic.com/v1/messages\","
                 "\"api_key\":\"sk-ant-x\",\"model\":\"claude\","
                 "\"system_prompt\":\"Be \\\"brief\\\".\"}", &config));
    assert(config.provider == ASSISTANT_PROVIDER_ANTHROPIC);
    assert(strcmp(config.api_key, "sk-ant-x") == 0);
    assert(strcmp(config.model, "claude") == 0);
    /* The old reader stopped at the first quote: "Be \". */
    assert(strcmp(config.system_prompt, "Be \"brief\".") == 0);
    assert(assistant_config_problem(&config) == NULL);

    assert(parse("{\"api_url\":\"http://10.0.0.2:11434/v1/chat/completions\","
                 "\"model\":\"llama3\"}", &config));
    assert(config.provider == ASSISTANT_PROVIDER_OPENAI);
    /* A local server needs no key. */
    assert(assistant_config_problem(&config) == NULL);
}

static void test_explicit_provider_and_defaults_fill_gaps(void)
{
    assistant_config_t config;
    assert(parse("{\"provider\":\"openai\",\"api_key\":\"sk-1\","
                 "\"model\":\"gpt\"}", &config));
    assert(config.provider == ASSISTANT_PROVIDER_OPENAI);
    assert(strcmp(config.api_url,
                  "https://api.openai.com/v1/chat/completions") == 0);

    /* Empty URL and model mean the default; an empty prompt means none. */
    assert(parse("{\"provider\":\"anthropic\",\"api_url\":\"\",\"model\":\"\","
                 "\"api_key\":\"k\",\"system_prompt\":\"\"}", &config));
    assert(strcmp(config.api_url, "https://api.anthropic.com/v1/messages") == 0);
    assert(config.model[0] != '\0');
    assert(config.system_prompt[0] == '\0');
}

static void test_malformed_documents_leave_defaults(void)
{
    assistant_config_t config;
    assert(!parse("", &config));
    assert(!parse("[\"api_key\"]", &config));
    assert(!parse("sk-ant-just-a-key", &config));
    assert(config.provider == ASSISTANT_PROVIDER_ANTHROPIC);
    assert(config.api_key[0] == '\0');
}

static void test_problems(void)
{
    assistant_config_t config;
    assistant_config_init(&config, ASSISTANT_PROVIDER_ANTHROPIC);
    strcpy(config.api_key, "k");
    assert(assistant_config_problem(&config) == NULL);

    static const char *const bad_urls[] = {
        "", "api.anthropic.com/v1/messages", "https://", "ftp://host/x",
        "https:///path",
    };
    for (size_t i = 0; i < sizeof(bad_urls) / sizeof(bad_urls[0]); i++) {
        strcpy(config.api_url, bad_urls[i]);
        assert(assistant_config_problem(&config) != NULL);
    }
    strcpy(config.api_url, "http://localhost:8080/v1/messages");
    assert(assistant_config_problem(&config) == NULL);
}

static void test_save_and_load_round_trip(void)
{
    char dir[] = "/tmp/assistant-config-XXXXXX";
    assert(mkdtemp(dir) != NULL);
    char path[128];
    snprintf(path, sizeof(path), "%s/config/assistant.json", dir);

    assistant_config_t saved;
    assistant_config_init(&saved, ASSISTANT_PROVIDER_OPENAI);
    strcpy(saved.api_key, "sk-proj-\"quoted\\key");
    strcpy(saved.model, "m\xc3\xa9");
    strcpy(saved.system_prompt, "Line one\nLine two\ttabbed");

    /* The directory is made if it is missing, and a second save replaces
     * the first. */
    assert(assistant_config_save(path, &saved));
    strcpy(saved.model, "model-2");
    assert(assistant_config_save(path, &saved));

    assistant_config_t loaded;
    assert(assistant_config_load(path, &loaded));
    assert(loaded.provider == ASSISTANT_PROVIDER_OPENAI);
    assert(strcmp(loaded.api_url, saved.api_url) == 0);
    assert(strcmp(loaded.api_key, saved.api_key) == 0);
    assert(strcmp(loaded.model, "model-2") == 0);
    assert(strcmp(loaded.system_prompt, saved.system_prompt) == 0);

    char temp[160];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    assert(access(temp, F_OK) != 0);

    assert(!assistant_config_load("/nonexistent/assistant.json", &loaded));
    assert(loaded.api_key[0] == '\0');

    remove(path);
    char config_dir[128];
    snprintf(config_dir, sizeof(config_dir), "%s/config", dir);
    rmdir(config_dir);
    rmdir(dir);
}

int main(void)
{
    test_defaults_per_provider();
    test_file_without_provider_is_inferred_from_its_url();
    test_explicit_provider_and_defaults_fill_gaps();
    test_malformed_documents_leave_defaults();
    test_problems();
    test_save_and_load_round_trip();
    puts("assistant config tests passed");
    return 0;
}
