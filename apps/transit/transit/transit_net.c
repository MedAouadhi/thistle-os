/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Transit app's HTTP. See transit_net.h.
 */
#include "transit/transit_net.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "thistle/net_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "transit";

#define MAX_REDIRECTS 5
#define USER_AGENT    "ThistleOS (+https://github.com/MedAouadhi/thistle-os)"

static void *psram_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(size);
}

static void *psram_realloc(void *ptr, size_t size)
{
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
    return p ? p : realloc(ptr, size);
}

static void fail(transit_error_t *error, int status, const char *words)
{
    error->http_status = status;
    snprintf(error->words, sizeof error->words, "%s", words);
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* Opens the request and reads its headers, following redirects: the final
 * status, or -1 when the connection failed. */
static int open_following(esp_http_client_handle_t client, int64_t *length)
{
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        if (esp_http_client_open(client, 0) != ESP_OK) return -1;
        int64_t n = esp_http_client_fetch_headers(client);
        if (n < 0) {
            esp_http_client_close(client);
            return -1;
        }
        int status = esp_http_client_get_status_code(client);
        if (!is_redirect(status)) {
            *length = esp_http_client_get_content_length(client);
            return status;
        }
        esp_err_t moved = esp_http_client_set_redirection(client);
        esp_http_client_close(client);
        if (moved != ESP_OK) return status;
    }
    return -1;
}

/* The URL up to its query, for the log. */
static void without_query(const char *url, char *out, size_t cap)
{
    const char *q = strchr(url, '?');
    int n = q ? (int)(q - url) : (int)strlen(url);
    snprintf(out, cap, "%.*s", n, url);
}

static const char *status_words(int status)
{
    if (status == 429) return "Too many requests to the transit service";
    if (status == 404) return "The transit service does not know this";
    if (status >= 500) return "Transit service not answering";
    return "The transit service gave an unexpected answer";
}

char *transit_net_get(const char *url, size_t *len, transit_error_t *error)
{
    *len = 0;
    char shown[TRANSIT_URL_MAX + 32];
    without_query(url, shown, sizeof shown);
    if (!net_is_connected()) {
        fail(error, 0, "No network");
        return NULL;
    }
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = TRANSIT_NET_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = USER_AGENT,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .max_redirection_count = MAX_REDIRECTS,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fail(error, 0, "Out of memory");
        return NULL;
    }
    esp_http_client_set_header(client, "Accept", "application/json");

    int64_t length = 0;
    int status = open_following(client, &length);
    if (status < 0) {
        esp_http_client_cleanup(client);
        ESP_LOGW(TAG, "GET %s: no answer", shown);
        fail(error, 0, "Transit service not answering");
        return NULL;
    }
    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ESP_LOGW(TAG, "GET %s: HTTP %d", shown, status);
        fail(error, status, status_words(status));
        return NULL;
    }

    size_t cap = length > 0 && length < TRANSIT_NET_BODY_MAX ? (size_t)length + 1 : 32 * 1024;
    char *body = psram_alloc(cap);
    size_t got = 0;
    bool dropped = false, too_big = false;
    while (body) {
        if (got + 1 >= cap) {
            if (cap >= TRANSIT_NET_BODY_MAX) {
                too_big = true;
                break;
            }
            size_t bigger = cap * 2 > TRANSIT_NET_BODY_MAX ? TRANSIT_NET_BODY_MAX : cap * 2;
            char *grown = psram_realloc(body, bigger);
            if (!grown) break;
            body = grown;
            cap = bigger;
        }
        int n = esp_http_client_read(client, body + got, (int)(cap - 1 - got));
        if (n < 0) dropped = true;
        if (n <= 0) break;
        got += (size_t)n;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!body) {
        fail(error, 0, "Out of memory");
        return NULL;
    }
    if (dropped || too_big) {
        free(body);
        ESP_LOGW(TAG, "GET %s: %s", shown, too_big ? "answer too long" : "connection dropped");
        fail(error, 200, too_big ? "The transit service's answer is too long"
                                 : "Transit service not answering");
        return NULL;
    }
    body[got] = '\0';
    *len = got;
    return body;
}
