// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors

#include "assistant_config.h"
#include "assistant_json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* A config is a few hundred bytes; this leaves room for a long system
 * prompt with every character escaped. */
#define CONFIG_FILE_MAX 8192

static const char DEFAULT_SYSTEM_PROMPT[] =
    "You are a helpful assistant on ThistleOS, running on a small handheld "
    "device. Keep answers short: they are read on a small screen.";

const char *assistant_provider_id(assistant_provider_t provider)
{
    return provider == ASSISTANT_PROVIDER_OPENAI ? "openai" : "anthropic";
}

const char *assistant_provider_default_url(assistant_provider_t provider)
{
    return provider == ASSISTANT_PROVIDER_OPENAI
        ? "https://api.openai.com/v1/chat/completions"
        : "https://api.anthropic.com/v1/messages";
}

const char *assistant_provider_default_model(assistant_provider_t provider)
{
    return provider == ASSISTANT_PROVIDER_OPENAI ? "" : "claude-haiku-5-5";
}

static void copy_text(char *out, size_t out_len, const char *text)
{
    snprintf(out, out_len, "%s", text);
}

void assistant_config_init(assistant_config_t *config,
                           assistant_provider_t provider)
{
    memset(config, 0, sizeof(*config));
    config->provider = provider;
    copy_text(config->api_url, sizeof(config->api_url),
              assistant_provider_default_url(provider));
    copy_text(config->model, sizeof(config->model),
              assistant_provider_default_model(provider));
    copy_text(config->system_prompt, sizeof(config->system_prompt),
              DEFAULT_SYSTEM_PROMPT);
}

/* Overwrite out with the string at key, if there is one. An empty string
 * counts only where keep_empty says it means something. */
static void read_field(const char *json, size_t len, const char *key,
                       char *out, size_t out_len, bool keep_empty)
{
    char *value = malloc(out_len);
    if (!value) {
        return;
    }
    if (assistant_json_get_string(json, len, key, value, out_len, NULL)
        && (keep_empty || value[0] != '\0')) {
        copy_text(out, out_len, value);
    }
    memset(value, 0, out_len);
    free(value);
}

static assistant_provider_t read_provider(const char *json, size_t len)
{
    if (assistant_json_string_equals(json, len, "provider", "openai")) {
        return ASSISTANT_PROVIDER_OPENAI;
    }
    if (assistant_json_string_equals(json, len, "provider", "anthropic")) {
        return ASSISTANT_PROVIDER_ANTHROPIC;
    }
    char url[ASSISTANT_URL_MAX];
    if (assistant_json_get_string(json, len, "api_url", url, sizeof(url), NULL)
        && strstr(url, "/chat/completions")) {
        return ASSISTANT_PROVIDER_OPENAI;
    }
    return ASSISTANT_PROVIDER_ANTHROPIC;
}

bool assistant_config_parse(const char *json, size_t len,
                            assistant_config_t *config)
{
    assistant_config_init(config, ASSISTANT_PROVIDER_ANTHROPIC);
    const char *root = assistant_json_find(json, len, "");
    if (!root || *root != '{') {
        return false;
    }

    assistant_config_init(config, read_provider(json, len));
    read_field(json, len, "api_url", config->api_url,
               sizeof(config->api_url), false);
    read_field(json, len, "api_key", config->api_key,
               sizeof(config->api_key), true);
    read_field(json, len, "model", config->model, sizeof(config->model),
               false);
    read_field(json, len, "system_prompt", config->system_prompt,
               sizeof(config->system_prompt), true);
    return true;
}

static bool write_field(assistant_json_writer_t *writer, const char *key,
                        const char *value, bool last)
{
    return assistant_json_write_raw(writer, "  \"")
        && assistant_json_write_raw(writer, key)
        && assistant_json_write_raw(writer, "\": ")
        && assistant_json_write_string(writer, value)
        && assistant_json_write_raw(writer, last ? "\n" : ",\n");
}

static bool write_config(assistant_json_writer_t *writer,
                         const assistant_config_t *config)
{
    return assistant_json_write_raw(writer, "{\n")
        && write_field(writer, "provider",
                       assistant_provider_id(config->provider), false)
        && write_field(writer, "api_url", config->api_url, false)
        && write_field(writer, "api_key", config->api_key, false)
        && write_field(writer, "model", config->model, false)
        && write_field(writer, "system_prompt", config->system_prompt, true)
        && assistant_json_write_raw(writer, "}\n")
        && writer->valid;
}

char *assistant_config_serialize(const assistant_config_t *config,
                                 size_t *out_length)
{
    if (!config || !out_length) {
        return NULL;
    }
    *out_length = 0;

    assistant_json_writer_t measure = { .valid = true };
    if (!write_config(&measure, config) || measure.length == SIZE_MAX) {
        return NULL;
    }
    char *json = malloc(measure.length + 1U);
    if (!json) {
        return NULL;
    }
    assistant_json_writer_t writer = {
        .buffer = json,
        .capacity = measure.length + 1U,
        .valid = true,
    };
    if (!write_config(&writer, config) || writer.length != measure.length) {
        free(json);
        return NULL;
    }
    json[writer.length] = '\0';
    *out_length = writer.length;
    return json;
}

static bool has_host(const char *url)
{
    static const char *const schemes[] = { "https://", "http://" };
    for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
        const size_t scheme_len = strlen(schemes[i]);
        if (strncmp(url, schemes[i], scheme_len) == 0) {
            return url[scheme_len] != '\0' && url[scheme_len] != '/';
        }
    }
    return false;
}

const char *assistant_config_problem(const assistant_config_t *config)
{
    if (!has_host(config->api_url)) {
        return "Enter a full URL, like https://host/path.";
    }
    if (config->model[0] == '\0') {
        return "Add a model name.";
    }
    /* Local OpenAI-style servers usually take no key. */
    if (config->provider == ASSISTANT_PROVIDER_ANTHROPIC
        && config->api_key[0] == '\0') {
        return "Add an API key.";
    }
    return NULL;
}

bool assistant_config_load(const char *path, assistant_config_t *config)
{
    assistant_config_init(config, ASSISTANT_PROVIDER_ANTHROPIC);
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    char *json = malloc(CONFIG_FILE_MAX + 1);
    if (!json) {
        fclose(file);
        return false;
    }
    const size_t length = fread(json, 1, CONFIG_FILE_MAX + 1, file);
    fclose(file);

    const bool ok = length <= CONFIG_FILE_MAX
        && assistant_config_parse(json, length, config);
    memset(json, 0, length);
    free(json);
    return ok;
}

bool assistant_config_save(const char *path, const assistant_config_t *config)
{
    char dir[ASSISTANT_URL_MAX];
    char temp[ASSISTANT_URL_MAX];
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir)
        || snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp)) {
        return false;
    }
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        mkdir(dir, 0755); /* fails harmlessly if it exists */
    }

    size_t length = 0;
    char *json = assistant_config_serialize(config, &length);
    if (!json) {
        return false;
    }

    FILE *file = fopen(temp, "wb");
    bool ok = file != NULL;
    if (file) {
        ok = fwrite(json, 1, length, file) == length;
        ok = fclose(file) == 0 && ok;
    }
    memset(json, 0, length);
    free(json);

    /* FAT will not rename over an existing file. */
    if (ok) {
        remove(path);
        ok = rename(temp, path) == 0;
    }
    if (!ok) {
        remove(temp);
    }
    return ok;
}
