#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*thistle_http_data_cb_t)(const uint8_t *data,
                                      int data_len,
                                      void *user_data);

/* A client whose perform() follows redirects on its own, as ESP-IDF's
 * does: up to ten, to any scheme. */
void *thistle_http_client_init(const char *url,
                               int timeout_ms,
                               thistle_http_data_cb_t data_cb,
                               void *user_data);
/* A client that follows no redirect on its own: a 3xx answer comes back
 * from perform() or fetch_headers(), and the caller follows it, if
 * redirect_policy.rs allows, with thistle_http_client_redirect(). */
void *thistle_http_client_init_download(const char *url,
                                        int timeout_ms,
                                        thistle_http_data_cb_t data_cb,
                                        void *user_data);
/* After a 3xx answer, points the client at its Location and writes where
 * that is to `url` (ESP-IDF's esp_http_client_get_url(): scheme, host,
 * port and path, no query). The connection is to be closed and opened, or
 * the request performed, again. */
int thistle_http_client_redirect(void *client, char *url, int length);
/* Make the request a POST of `length` bytes of `content_type` from `body`,
 * which must outlive perform(). */
int thistle_http_client_set_post(void *client,
                                 const char *content_type,
                                 const char *body,
                                 int length);
int thistle_http_client_perform(void *client);
int thistle_http_client_get_status_code(void *client);
int thistle_http_client_open(void *client, int write_len);
int64_t thistle_http_client_fetch_headers(void *client);
int thistle_http_client_read(void *client, char *buffer, int length);
int thistle_http_client_close(void *client);
int thistle_http_client_cleanup(void *client);

#ifdef __cplusplus
}
#endif
