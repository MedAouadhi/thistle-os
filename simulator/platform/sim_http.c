/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS simulator — HTTP client using libcurl (macOS host build).
 *
 * Provides both a buffered API (sim_http_client_perform) and a streaming
 * read API (open / fetch_headers / read / close) that mirrors the
 * esp_http_client chunked-read pattern used by appstore_download_file.
 */

#include "sim_http.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct sim_http_client {
    char   url[512];
    int    timeout_ms;
    bool   follow_redirects;
    void  *event_handler;
    void  *user_data;
    char   method[8];
    struct curl_slist *headers;
    char  *post_data;
    size_t post_len;
    char  *response_buf;
    size_t response_len;
    size_t response_cap;
    int    status_code;
    int    content_length;
    /* Read cursor for streaming API */
    size_t read_offset;
};

/* ── Write callback — appends received data to the response buffer ── */

static size_t write_callback(void *data, size_t size, size_t nmemb, void *userp)
{
    struct sim_http_client *c = (struct sim_http_client *)userp;
    size_t total = size * nmemb;

    if (c->response_len + total >= c->response_cap) {
        size_t new_cap = c->response_cap * 2 + total;
        char *new_buf = realloc(c->response_buf, new_cap);
        if (!new_buf) return 0;
        c->response_buf = new_buf;
        c->response_cap = new_cap;
    }

    memcpy(c->response_buf + c->response_len, data, total);
    c->response_len += total;
    c->response_buf[c->response_len] = '\0';
    return total;
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

sim_http_client_handle_t sim_http_client_init(const sim_http_client_config_t *config)
{
    if (!config || !config->url) return NULL;

    struct sim_http_client *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    strncpy(c->url, config->url, sizeof(c->url) - 1);
    c->timeout_ms   = config->timeout_ms > 0 ? config->timeout_ms : 15000;
    c->follow_redirects = !config->disable_auto_redirect;
    c->event_handler = config->event_handler;
    c->user_data     = config->user_data;
    strcpy(c->method, "GET");
    c->response_cap = 4096;
    c->response_buf = malloc(c->response_cap);
    if (!c->response_buf) { free(c); return NULL; }
    c->response_buf[0] = '\0';
    return c;
}

void sim_http_client_cleanup(sim_http_client_handle_t client)
{
    if (!client) return;
    curl_slist_free_all(client->headers);
    free(client->post_data);
    free(client->response_buf);
    free(client);
}

/* ── Request setup ────────────────────────────────────────────────── */

esp_err_t sim_http_client_set_method(sim_http_client_handle_t client, const char *method)
{
    if (!client || !method || strlen(method) >= sizeof(client->method)) return ESP_FAIL;
    strcpy(client->method, method);
    return ESP_OK;
}

esp_err_t sim_http_client_set_header(sim_http_client_handle_t client,
                                     const char *key, const char *value)
{
    if (!client || !key || !value) return ESP_FAIL;
    size_t len = strlen(key) + strlen(value) + 3;
    char *line = malloc(len);
    if (!line) return ESP_FAIL;
    snprintf(line, len, "%s: %s", key, value);
    struct curl_slist *headers = curl_slist_append(client->headers, line);
    free(line);
    if (!headers) return ESP_FAIL;
    client->headers = headers;
    return ESP_OK;
}

esp_err_t sim_http_client_set_post_field(sim_http_client_handle_t client,
                                         const char *data, int len)
{
    if (!client || len < 0 || (len > 0 && !data)) return ESP_FAIL;
    char *copy = malloc((size_t)len + 1);
    if (!copy) return ESP_FAIL;
    if (len > 0) memcpy(copy, data, (size_t)len);
    copy[len] = '\0';
    free(client->post_data);
    client->post_data = copy;
    client->post_len  = (size_t)len;
    return ESP_OK;
}

void *sim_http_client_get_event_handler(sim_http_client_handle_t client)
{
    return client ? client->event_handler : NULL;
}

void *sim_http_client_get_user_data(sim_http_client_handle_t client)
{
    return client ? client->user_data : NULL;
}

/* ── Buffered perform ─────────────────────────────────────────────── */

esp_err_t sim_http_client_perform(sim_http_client_handle_t client)
{
    if (!client) return ESP_FAIL;

    /* Reset buffer for a fresh request */
    client->response_len  = 0;
    client->read_offset   = 0;
    client->response_buf[0] = '\0';

    CURL *curl = curl_easy_init();
    if (!curl) return ESP_FAIL;

    curl_easy_setopt(curl, CURLOPT_URL,           client->url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      client);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,    (long)client->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, client->follow_redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    /* An empty "Expect:" stops curl holding a large body back for a
     * 100-continue that esp_http_client never waits for either. */
    struct curl_slist *headers = client->headers;
    struct curl_slist *with_expect = NULL;
    if (client->post_data) {
        with_expect = curl_slist_append(NULL, "Expect:");
        for (struct curl_slist *h = client->headers; h && with_expect; h = h->next) {
            struct curl_slist *next = curl_slist_append(with_expect, h->data);
            if (!next) { curl_slist_free_all(with_expect); with_expect = NULL; break; }
            with_expect = next;
        }
        headers = with_expect;
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (client->post_data) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    client->post_data);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)client->post_len);
    }
    if (strcmp(client->method, "POST") == 0 && !client->post_data) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    } else if (strcmp(client->method, "GET") != 0 && strcmp(client->method, "POST") != 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, client->method);
    }

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(with_expect);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    client->status_code = (int)http_code;

    curl_off_t cl = -1;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
    client->content_length = (cl > 0) ? (int)cl : (int)client->response_len;

    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[sim_http] curl error: %s\n", curl_easy_strerror(res));
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ── Accessors ────────────────────────────────────────────────────── */

int sim_http_client_get_status_code(sim_http_client_handle_t c) {
    return c ? c->status_code : 0;
}

int sim_http_client_get_content_length(sim_http_client_handle_t c) {
    return c ? c->content_length : 0;
}

const char *sim_http_client_get_response_data(sim_http_client_handle_t c) {
    return c ? c->response_buf : NULL;
}

size_t sim_http_client_get_response_length(sim_http_client_handle_t c) {
    return c ? c->response_len : 0;
}

/* ── Streaming API (open / fetch_headers / read / close) ─────────── *
 *
 * appstore_download_file uses this pattern to stream a file to disk
 * without buffering the whole thing in RAM.  On the simulator we still
 * buffer it (memory is plentiful on the host), but we expose the same
 * chunked-read interface so the calling code compiles unchanged.
 */

esp_err_t sim_http_client_open(sim_http_client_handle_t client, int write_len)
{
    (void)write_len;
    if (!client) return ESP_FAIL;
    /* Reset read cursor; actual transfer happens in fetch_headers */
    client->read_offset = 0;
    return ESP_OK;
}

int sim_http_client_fetch_headers(sim_http_client_handle_t client)
{
    if (!client) return -1;
    /* Perform the full HTTP request and buffer the body */
    esp_err_t ret = sim_http_client_perform(client);
    if (ret != ESP_OK) return -1;
    client->content_length = (int)client->response_len;
    return client->content_length;
}

int sim_http_client_read(sim_http_client_handle_t client, char *buf, int len)
{
    if (!client || !buf || client->read_offset >= client->response_len) return 0;
    size_t remaining = client->response_len - client->read_offset;
    size_t to_copy   = ((size_t)len < remaining) ? (size_t)len : remaining;
    memcpy(buf, client->response_buf + client->read_offset, to_copy);
    client->read_offset += to_copy;
    return (int)to_copy;
}

esp_err_t sim_http_client_close(sim_http_client_handle_t client)
{
    if (client) client->read_offset = 0;
    return ESP_OK;
}
