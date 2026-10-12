#pragma once

/* Simulator shim — maps esp_http_client API to sim_http using libcurl.
 *
 * This header is picked up by the simulator build BEFORE any ESP-IDF
 * headers (the platform/ directory is first in the include path), so
 * appstore_client.c and the Assistant compile without any #ifdef guards.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sim_http.h"

typedef sim_http_client_handle_t esp_http_client_handle_t;

typedef esp_err_t (*crt_bundle_attach_fn)(void *);

typedef enum {
    HTTP_EVENT_ON_DATA = 0,
} esp_http_client_event_id_t;

typedef enum {
    HTTP_METHOD_GET  = 0,
    HTTP_METHOD_POST = 1,
    HTTP_METHOD_PUT  = 2,
    HTTP_METHOD_DELETE = 3,
} esp_http_client_method_t;

typedef struct {
    esp_http_client_event_id_t event_id;
    void *data;
    int   data_len;
    void *user_data;
} esp_http_client_event_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);

typedef struct {
    const char *url;
    /* Given the whole body in one HTTP_EVENT_ON_DATA once the request
     * completes, where ESP-IDF streams it in chunks. */
    http_event_handle_cb event_handler;
    void       *user_data;
    crt_bundle_attach_fn crt_bundle_attach; /* ignored in simulator */
    int         timeout_ms;
    bool        disable_auto_redirect;
    int         buffer_size_tx;             /* ignored in simulator */
} esp_http_client_config_t;

static inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    sim_http_client_config_t sc = {
        .url        = config->url,
        .timeout_ms = config->timeout_ms,
        .disable_auto_redirect = config->disable_auto_redirect,
        .event_handler = (void *)config->event_handler,
        .user_data  = config->user_data,
    };
    return sim_http_client_init(&sc);
}

static inline esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
    esp_err_t err = sim_http_client_perform(c);
    http_event_handle_cb handler =
        (http_event_handle_cb)sim_http_client_get_event_handler(c);
    size_t length = sim_http_client_get_response_length(c);
    if (err == ESP_OK && handler && length > 0) {
        esp_http_client_event_t event = {
            .event_id  = HTTP_EVENT_ON_DATA,
            .data      = (void *)sim_http_client_get_response_data(c),
            .data_len  = (int)length,
            .user_data = sim_http_client_get_user_data(c),
        };
        handler(&event);
    }
    return err;
}

static inline int esp_http_client_get_status_code(esp_http_client_handle_t c) {
    return sim_http_client_get_status_code(c);
}

static inline esp_err_t esp_http_client_open(esp_http_client_handle_t c, int len) {
    return sim_http_client_open(c, len);
}

static inline int esp_http_client_fetch_headers(esp_http_client_handle_t c) {
    return sim_http_client_fetch_headers(c);
}

static inline int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len) {
    return sim_http_client_read(c, buf, len);
}

static inline esp_err_t esp_http_client_close(esp_http_client_handle_t c) {
    return sim_http_client_close(c);
}

static inline void esp_http_client_cleanup(esp_http_client_handle_t c) {
    sim_http_client_cleanup(c);
}

static inline esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, esp_http_client_method_t m) {
    static const char *const names[] = { "GET", "POST", "PUT", "DELETE" };
    if ((unsigned)m >= sizeof(names) / sizeof(names[0])) return ESP_FAIL;
    return sim_http_client_set_method(c, names[m]);
}

static inline esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *val) {
    return sim_http_client_set_header(c, key, val);
}

static inline esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *data, int len) {
    return sim_http_client_set_post_field(c, data, len);
}
