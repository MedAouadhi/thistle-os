/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Podcasts app's HTTP.
 */
#include "podcasts/pod_net.h"
#include "podcasts/pod_api.h"
#include "podcasts/pod_cred.h"
#include "podcasts/pod_json.h"
#include "podcasts/pod_text.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "thistle/net_manager.h"
#include "thistle/time_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "pod_net";

/* An API answer is a few tens of KB; a search for something common, with
 * descriptions, under 200. */
#define API_BODY_MAX  (512 * 1024)
#define MAX_REDIRECTS 10

struct pod_stream {
    esp_http_client_handle_t client;
};

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

bool pod_net_connected(void)
{
    return net_is_connected();
}

static void set_error(char *error, size_t cap, const char *text)
{
    if (error && cap) pod_text_copy(error, cap, text);
}

static bool is_redirect(int status)
{
    return status == HttpStatus_MovedPermanently || status == HttpStatus_Found ||
           status == HttpStatus_SeeOther || status == HttpStatus_TemporaryRedirect ||
           status == HttpStatus_PermanentRedirect;
}

/* Opens the client's request and reads its headers, following redirects:
 * the final status, or -1 when the connection failed. */
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
        /* The next hop may be another host, or this one again; a fresh
         * connection either way, so nothing of this answer's body is left
         * to read first. */
        if (esp_http_client_set_redirection(client) != ESP_OK) {
            esp_http_client_close(client);
            return status;
        }
        esp_http_client_close(client);
    }
    return -1;
}

static esp_http_client_handle_t new_client(const char *url)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = POD_NET_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = POD_USER_AGENT,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,   /* enclosure URLs run long */
        .max_redirection_count = MAX_REDIRECTS,
        .disable_auto_redirect = true,
    };
    return esp_http_client_init(&config);
}

/* ------------------------------------------------------------------ */
/* The API                                                              */
/* ------------------------------------------------------------------ */

static const char *status_words(int status)
{
    switch (status) {
    case 401:
    case 403: return "PodcastIndex refused the key and secret";
    case 404: return "Not found on PodcastIndex";
    case 429: return "PodcastIndex says too many requests; wait a little";
    default:  return status >= 500 ? "PodcastIndex is having trouble; try again later"
                                   : "PodcastIndex gave an unexpected answer";
    }
}

/* What a refusal says of itself: PodcastIndex answers 4xx with
 * {"status": "false", "description": "..."}; anything else, ours. */
static void refusal_words(const char *body, size_t len, int status, char *error, size_t error_cap)
{
    char description[128] = "", key[16];
    pod_json_t j;
    pod_json_init(&j, body, len);
    if (status < 500 && pod_json_object(&j)) {
        while (pod_json_key(&j, key, sizeof key)) {
            if (strcmp(key, "description") == 0) pod_json_string(&j, description, sizeof description);
            else pod_json_skip(&j);
        }
    }
    set_error(error, error_cap, description[0] && !j.error ? description : status_words(status));
}

char *pod_net_api_get(const char *path, size_t *len, char *error, size_t error_cap)
{
    *len = 0;
    if (!net_is_connected()) {
        set_error(error, error_cap, "No network. Connect to WiFi or turn on mobile data.");
        return NULL;
    }
    /* The date is signed, and PodcastIndex refuses one more than three
     * minutes from its own. */
    time_status_t ts;
    if (time_manager_get_status(&ts) != ESP_OK || !ts.known) {
        set_error(error, error_cap, "The clock is not set yet. Try again once it is.");
        return NULL;
    }
    char key[POD_CRED_KEY_MAX], secret[POD_CRED_SECRET_MAX];
    if (!pod_cred_get(key, sizeof key, secret, sizeof secret)) {
        set_error(error, error_cap, "No PodcastIndex key. Set one up from the home page.");
        return NULL;
    }
    char date[24], auth[41];
    pod_api_auth(key, secret, (int64_t)time(NULL), date, auth);
    memset(secret, 0, sizeof secret);

    char base[192];
    pod_cred_api_base(base, sizeof base);
    char *url = psram_alloc(sizeof base + strlen(path) + 1);
    if (!url) {
        memset(key, 0, sizeof key);
        set_error(error, error_cap, "Out of memory");
        return NULL;
    }
    sprintf(url, "%s%s", base, path);
    esp_http_client_handle_t client = new_client(url);
    free(url);
    if (!client) {
        memset(key, 0, sizeof key);
        set_error(error, error_cap, "Out of memory");
        return NULL;
    }
    esp_http_client_set_header(client, "X-Auth-Key", key);
    esp_http_client_set_header(client, "X-Auth-Date", date);
    esp_http_client_set_header(client, "Authorization", auth);

    int64_t length = 0;
    int status = open_following(client, &length);
    char *body = NULL;
    if (status < 0) {
        set_error(error, error_cap, "Could not reach PodcastIndex");
    } else {
        /* Read the body even when the status is not 200: PodcastIndex says
         * what was wrong in it. */
        size_t cap = length > 0 && length < API_BODY_MAX ? (size_t)length + 1 : 16 * 1024;
        body = psram_alloc(cap);
        size_t got = 0;
        while (body) {
            if (got + 1 >= cap) {
                if (cap >= API_BODY_MAX) break;
                size_t bigger = cap * 2 > API_BODY_MAX ? API_BODY_MAX : cap * 2;
                char *grown = psram_realloc(body, bigger);
                if (!grown) break;
                body = grown;
                cap = bigger;
            }
            int n = esp_http_client_read(client, body + got, (int)(cap - 1 - got));
            if (n <= 0) {
                if (n < 0) status = -1;
                break;
            }
            got += (size_t)n;
        }
        if (!body) {
            set_error(error, error_cap, "Out of memory");
        } else if (status < 0) {
            set_error(error, error_cap, "The connection to PodcastIndex dropped");
            free(body);
            body = NULL;
        } else {
            body[got] = '\0';
            *len = got;
            if (status != 200) {
                ESP_LOGW(TAG, "%s: HTTP %d", path, status);
                refusal_words(body, got, status, error, error_cap);
                free(body);
                body = NULL;
                *len = 0;
            }
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    memset(key, 0, sizeof key);
    return body;
}

/* ------------------------------------------------------------------ */
/* Streams                                                              */
/* ------------------------------------------------------------------ */

pod_stream_t *pod_net_open(const char *url, int64_t from, int64_t *total, int64_t *start,
                           char *error, size_t error_cap)
{
    *total = 0;
    *start = 0;
    if (!net_is_connected()) {
        set_error(error, error_cap, "No network");
        return NULL;
    }
    pod_stream_t *s = calloc(1, sizeof *s);
    if (!s) {
        set_error(error, error_cap, "Out of memory");
        return NULL;
    }
    s->client = new_client(url);
    if (!s->client) {
        free(s);
        set_error(error, error_cap, "Out of memory");
        return NULL;
    }
    if (from > 0) {
        char range[48];
        snprintf(range, sizeof range, "bytes=%" PRId64 "-", from);
        esp_http_client_set_header(s->client, "Range", range);
    }

    int64_t length = 0;
    int status = open_following(s->client, &length);
    if (status == 206) {
        *start = from;
        *total = length > 0 ? from + length : 0;
        return s;
    }
    if (status == 200) {
        /* The whole file, the range ignored: the caller skips what it does
         * not want. */
        *start = 0;
        *total = length > 0 ? length : 0;
        return s;
    }

    if (status == 416) set_error(error, error_cap, POD_NET_PAST_END);
    else if (status == 404 || status == 410) set_error(error, error_cap, "The episode's file is gone from its host");
    else if (status < 0) set_error(error, error_cap, "Could not reach the episode's host");
    else {
        char words[48];
        snprintf(words, sizeof words, "The episode's host answered %d", status);
        set_error(error, error_cap, words);
    }
    ESP_LOGW(TAG, "open: HTTP %d", status);
    if (status >= 0) esp_http_client_close(s->client);
    esp_http_client_cleanup(s->client);
    free(s);
    return NULL;
}

int pod_net_read(pod_stream_t *s, void *buf, int len)
{
    return esp_http_client_read(s->client, buf, len);
}

void pod_net_close(pod_stream_t *s)
{
    if (!s) return;
    esp_http_client_close(s->client);
    esp_http_client_cleanup(s->client);
    free(s);
}
