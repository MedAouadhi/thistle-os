/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS simulator — HTTP client using libcurl (macOS host build).
 *
 * Provides both a buffered API (sim_http_client_perform) and a streaming
 * read API (open / fetch_headers / read / close) that mirrors the
 * esp_http_client chunked-read pattern used by appstore_download_file and
 * the Podcasts app's player and downloads.
 */

#include "sim_http.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>

struct sim_http_client {
    char  *url;
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
    /* The streaming API's transfer, driven a step at a time by read() */
    CURLM *multi;
    CURL  *easy;
    bool   stream_done;      /* the transfer has finished, well or not */
    bool   stream_failed;
    char  *location;         /* a redirect's Location, for set_redirection() */
    char  *location_raw;     /* as the header gave it, maybe relative */
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

    c->url = strdup(config->url);
    if (!c->url) { free(c); return NULL; }
    c->timeout_ms   = config->timeout_ms > 0 ? config->timeout_ms : 15000;
    c->follow_redirects = !config->disable_auto_redirect;
    c->event_handler = config->event_handler;
    c->user_data     = config->user_data;
    strcpy(c->method, "GET");
    c->response_cap = 4096;
    c->response_buf = malloc(c->response_cap);
    if (!c->response_buf) { free(c->url); free(c); return NULL; }
    c->response_buf[0] = '\0';
    return c;
}

void sim_http_client_cleanup(sim_http_client_handle_t client)
{
    if (!client) return;
    sim_http_client_close(client);
    free(client->url);
    free(client->location);
    free(client->location_raw);
    curl_slist_free_all(client->headers);
    free(client->post_data);
    free(client->response_buf);
    free(client);
}

/* ── Request setup ────────────────────────────────────────────────── */

esp_err_t sim_http_client_set_url(sim_http_client_handle_t client, const char *url)
{
    if (!client || !url) return ESP_FAIL;
    char *copy = strdup(url);
    if (!copy) return ESP_FAIL;
    free(client->url);
    client->url = copy;
    return ESP_OK;
}

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

static size_t header_callback(char *data, size_t size, size_t nmemb, void *userp);
static void resolve_location(struct sim_http_client *client);

/* A transfer of the request as set up so far, the body going to
 * write_callback. */
static CURL *new_easy(struct sim_http_client *client)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    curl_easy_setopt(curl, CURLOPT_URL,           client->url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      client);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, client->follow_redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,      10L);  /* ESP-IDF's default */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    /* A test's own certificate authority, so a fake server can speak HTTPS
     * (test_system_update.py's redirect from HTTPS to HTTP). */
    const char *ca = getenv("THISTLE_SIM_CA_BUNDLE");
    if (ca && ca[0]) curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
    return curl;
}

/* ── Buffered perform ─────────────────────────────────────────────── */

esp_err_t sim_http_client_perform(sim_http_client_handle_t client)
{
    if (!client) return ESP_FAIL;

    /* Reset buffer for a fresh request */
    client->response_len  = 0;
    client->read_offset   = 0;
    client->response_buf[0] = '\0';

    CURL *curl = new_easy(client);
    if (!curl) return ESP_FAIL;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,    (long)client->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA,     client);
    free(client->location_raw);
    client->location_raw = NULL;

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
    resolve_location(client);

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
 * As on the device, the body arrives as it is read: open() starts the
 * transfer, fetch_headers() runs it until the headers are in, and each read()
 * runs it until there is some body to hand over. A podcast is never held
 * whole in memory, and a 50 MB episode starts playing at once.
 *
 * timeout_ms is how long any one step may wait, as ESP-IDF's is per socket
 * operation; the whole transfer may take as long as it takes.
 */

/* Keeps the Location header, which ESP-IDF does too, for a redirect the
 * caller follows itself. */
static size_t header_callback(char *data, size_t size, size_t nmemb, void *userp)
{
    struct sim_http_client *c = (struct sim_http_client *)userp;
    size_t len = size * nmemb;
    if (len >= 9 && strncasecmp(data, "location:", 9) == 0) {
        size_t start = 9, end = len;
        while (start < end && (data[start] == ' ' || data[start] == '\t')) start++;
        while (end > start && (data[end - 1] == '\r' || data[end - 1] == '\n' || data[end - 1] == ' ')) end--;
        free(c->location_raw);
        c->location_raw = strndup(data + start, end - start);
    }
    return len;
}

/* Runs the transfer for up to wait_ms, or until it has finished. */
static void stream_step(struct sim_http_client *c, int wait_ms)
{
    int running = 0;
    if (curl_multi_perform(c->multi, &running) != CURLM_OK) {
        c->stream_done = c->stream_failed = true;
        return;
    }
    if (running) {
        if (wait_ms > 0) curl_multi_poll(c->multi, NULL, 0, wait_ms, NULL);
        return;
    }

    int left = 0;
    CURLMsg *msg;
    while ((msg = curl_multi_info_read(c->multi, &left)) != NULL) {
        if (msg->msg == CURLMSG_DONE && msg->data.result != CURLE_OK) {
            fprintf(stderr, "[sim_http] curl error: %s\n", curl_easy_strerror(msg->data.result));
            c->stream_failed = true;
        }
    }
    c->stream_done = true;
}

/* Runs the transfer until it has body to hand over or has finished: true,
 * or false when timeout_ms passed with neither. */
static bool stream_wait(struct sim_http_client *c)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!c->stream_done && c->read_offset >= c->response_len) {
        stream_step(c, 50);
        clock_gettime(CLOCK_MONOTONIC, &now);
        long waited = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (waited >= c->timeout_ms) return false;
    }
    return true;
}

esp_err_t sim_http_client_open(sim_http_client_handle_t client, int write_len)
{
    (void)write_len;
    if (!client) return ESP_FAIL;
    sim_http_client_close(client);

    client->response_len = 0;
    client->read_offset = 0;
    client->status_code = 0;
    client->content_length = 0;
    client->stream_done = client->stream_failed = false;

    client->easy = new_easy(client);
    client->multi = curl_multi_init();
    if (!client->easy || !client->multi) {
        sim_http_client_close(client);
        return ESP_FAIL;
    }
    long wait_s = client->timeout_ms / 1000 > 0 ? client->timeout_ms / 1000 : 1;
    curl_easy_setopt(client->easy, CURLOPT_CONNECTTIMEOUT_MS, (long)client->timeout_ms);
    curl_easy_setopt(client->easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(client->easy, CURLOPT_LOW_SPEED_TIME, wait_s);
    curl_easy_setopt(client->easy, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(client->easy, CURLOPT_HEADERDATA, client);
    free(client->location_raw);
    client->location_raw = NULL;
    if (client->headers) curl_easy_setopt(client->easy, CURLOPT_HTTPHEADER, client->headers);
    if (strcmp(client->method, "GET") != 0) {
        curl_easy_setopt(client->easy, CURLOPT_CUSTOMREQUEST, client->method);
    }
    curl_multi_add_handle(client->multi, client->easy);
    return ESP_OK;
}

int sim_http_client_fetch_headers(sim_http_client_handle_t client)
{
    if (!client || !client->multi) return -1;
    if (!stream_wait(client)) return -ESP_ERR_HTTP_EAGAIN;
    if (client->stream_failed) return -1;

    long http_code = 0;
    curl_easy_getinfo(client->easy, CURLINFO_RESPONSE_CODE, &http_code);
    client->status_code = (int)http_code;
    curl_off_t cl = -1;
    curl_easy_getinfo(client->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
    /* As ESP-IDF: 0 when the server did not say. */
    client->content_length = cl > 0 ? (int)cl : 0;
    resolve_location(client);
    return client->content_length;
}

/* Where a redirect not followed points, made whole if relative. */
static void resolve_location(struct sim_http_client *client)
{
    free(client->location);
    client->location = NULL;
    if (client->location_raw) {
        CURLU *u = curl_url();
        char *whole = NULL;
        if (u && curl_url_set(u, CURLUPART_URL, client->url, 0) == CURLUE_OK &&
            curl_url_set(u, CURLUPART_URL, client->location_raw, 0) == CURLUE_OK &&
            curl_url_get(u, CURLUPART_URL, &whole, 0) == CURLUE_OK) {
            client->location = strdup(whole);
        }
        curl_free(whole);
        curl_url_cleanup(u);
    }
}

esp_err_t sim_http_client_set_redirection(sim_http_client_handle_t client)
{
    if (!client || !client->location) return ESP_ERR_INVALID_ARG;
    return sim_http_client_set_url(client, client->location);
}

esp_err_t sim_http_client_get_url(sim_http_client_handle_t client, char *url, int len)
{
    if (!client || !url || len <= 0 || !client->url) return ESP_ERR_INVALID_ARG;
    snprintf(url, (size_t)len, "%s", client->url);
    return ESP_OK;
}

int sim_http_client_read(sim_http_client_handle_t client, char *buf, int len)
{
    if (!client || !buf || len <= 0) return 0;
    if (!client->multi) {
        return 0;
    }
    if (!stream_wait(client)) return -ESP_ERR_HTTP_EAGAIN;

    size_t remaining = client->response_len - client->read_offset;
    if (remaining == 0) return client->stream_failed ? -1 : 0;
    size_t to_copy = ((size_t)len < remaining) ? (size_t)len : remaining;
    memcpy(buf, client->response_buf + client->read_offset, to_copy);
    client->read_offset += to_copy;
    if (client->read_offset == client->response_len) {
        /* All handed over: the buffer starts again, so it never grows past
         * what one step of the transfer delivers. */
        client->read_offset = client->response_len = 0;
    }
    return (int)to_copy;
}

esp_err_t sim_http_client_close(sim_http_client_handle_t client)
{
    if (!client) return ESP_OK;
    if (client->multi && client->easy) curl_multi_remove_handle(client->multi, client->easy);
    if (client->easy) curl_easy_cleanup(client->easy);
    if (client->multi) curl_multi_cleanup(client->multi);
    client->easy = NULL;
    client->multi = NULL;
    client->read_offset = 0;
    return ESP_OK;
}
