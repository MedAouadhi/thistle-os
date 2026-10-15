/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS Simulator — ESP-IDF's HTTP server, on the host's sockets
 *
 * One thread accepts connections on 127.0.0.1 and answers one request on
 * each, calling the handler registered for its URI and method as ESP-IDF's
 * server would, then closes it. See esp_http_server.h.
 */
#include "esp_http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WASM_BUILD

/* The browser simulator has no sockets to listen on. */
esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    (void)config;
    *handle = NULL;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t httpd_stop(httpd_handle_t handle) { (void)handle; return ESP_OK; }
esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u) { (void)h; (void)u; return ESP_FAIL; }
int httpd_req_to_sockfd(httpd_req_t *r) { (void)r; return -1; }
int httpd_req_recv(httpd_req_t *r, char *b, size_t n) { (void)r; (void)b; (void)n; return HTTPD_SOCK_ERR_FAIL; }
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *f) { (void)r; (void)f; return 0; }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *f, char *v, size_t n) { (void)r; (void)f; (void)v; (void)n; return ESP_ERR_NOT_FOUND; }
size_t httpd_req_get_url_query_len(httpd_req_t *r) { (void)r; return 0; }
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *b, size_t n) { (void)r; (void)b; (void)n; return ESP_ERR_NOT_FOUND; }
esp_err_t httpd_query_key_value(const char *q, const char *k, char *v, size_t n) { (void)q; (void)k; (void)v; (void)n; return ESP_ERR_NOT_FOUND; }
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { (void)r; (void)s; return ESP_FAIL; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t) { (void)r; (void)t; return ESP_FAIL; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *f, const char *v) { (void)r; (void)f; (void)v; return ESP_FAIL; }
esp_err_t httpd_resp_send(httpd_req_t *r, const char *b, ssize_t n) { (void)r; (void)b; (void)n; return ESP_FAIL; }
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *b, ssize_t n) { (void)r; (void)b; (void)n; return ESP_FAIL; }
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t e, const char *m) { (void)r; (void)e; (void)m; return ESP_FAIL; }
int httpd_send(httpd_req_t *r, const char *b, size_t n) { (void)r; (void)b; (void)n; return HTTPD_SOCK_ERR_FAIL; }
uint16_t sim_httpd_port(httpd_handle_t h) { (void)h; return 0; }

#else

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define HEAD_MAX     8192
#define MAX_HEADERS  64
#define MAX_RESP_HDR 32

typedef struct {
    int            listen_fd;
    uint16_t       port;
    pthread_t      thread;
    volatile bool  stop;
    httpd_config_t config;
    httpd_uri_t   *routes;
    int            route_count;
} server_t;

/* The request being answered, behind httpd_req_t.aux. */
typedef struct {
    int         fd;
    char        head[HEAD_MAX + 1];
    size_t      head_len;      /* bytes of `head` used, the body's first ones too */
    size_t      body_at;       /* where the body starts in `head` */
    size_t      body_read;     /* of content_len, handed out so far */
    const char *query;         /* in uri, past '?', or NULL */
    const char *names[MAX_HEADERS];
    const char *values[MAX_HEADERS];
    int         header_count;

    const char *status;
    const char *type;
    const char *resp_names[MAX_RESP_HDR];
    const char *resp_values[MAX_RESP_HDR];
    int         resp_count;
    bool        head_sent;     /* the status line and headers */
    bool        chunked;
    bool        failed;
} req_aux_t;

static const char *const METHOD_NAMES[] = {
    [HTTP_DELETE] = "DELETE", [HTTP_GET] = "GET",   [HTTP_HEAD] = "HEAD",
    [HTTP_POST] = "POST",     [HTTP_PUT] = "PUT",   [HTTP_OPTIONS] = "OPTIONS",
    [HTTP_PATCH] = "PATCH",
};

static int method_of(const char *name)
{
    for (size_t i = 0; i < sizeof METHOD_NAMES / sizeof METHOD_NAMES[0]; i++) {
        if (METHOD_NAMES[i] && strcmp(METHOD_NAMES[i], name) == 0) return (int)i;
    }
    return -1;
}

static bool write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

static req_aux_t *aux_of(httpd_req_t *r)
{
    return r ? (req_aux_t *)r->aux : NULL;
}

/* ------------------------------------------------------------------ */
/* Reading the request                                                  */
/* ------------------------------------------------------------------ */

static bool read_head(req_aux_t *a)
{
    while (a->head_len < HEAD_MAX) {
        ssize_t n = recv(a->fd, a->head + a->head_len, HEAD_MAX - a->head_len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        a->head_len += (size_t)n;
        a->head[a->head_len] = '\0';
        char *end = strstr(a->head, "\r\n\r\n");
        if (end) {
            a->body_at = (size_t)(end - a->head) + 4;
            return true;
        }
    }
    return false;
}

static void parse_headers(req_aux_t *a, char *lines)
{
    char *save = NULL;
    for (char *line = strtok_r(lines, "\r\n", &save); line && a->header_count < MAX_HEADERS;
         line = strtok_r(NULL, "\r\n", &save)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t') value++;
        a->names[a->header_count] = line;
        a->values[a->header_count] = value;
        a->header_count++;
    }
}

static const char *header(req_aux_t *a, const char *name)
{
    for (int i = 0; i < a->header_count; i++) {
        if (strcasecmp(a->names[i], name) == 0) return a->values[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Writing the answer                                                   */
/* ------------------------------------------------------------------ */

static bool send_head(req_aux_t *a, long long length)
{
    char head[4096];
    int n = snprintf(head, sizeof head, "HTTP/1.1 %s\r\nContent-Type: %s\r\nConnection: close\r\n",
                     a->status ? a->status : HTTPD_200, a->type ? a->type : HTTPD_TYPE_TEXT);
    for (int i = 0; i < a->resp_count && n > 0 && (size_t)n < sizeof head; i++) {
        n += snprintf(head + n, sizeof head - (size_t)n, "%s: %s\r\n", a->resp_names[i],
                      a->resp_values[i]);
    }
    if (n > 0 && (size_t)n < sizeof head) {
        if (length >= 0) {
            n += snprintf(head + n, sizeof head - (size_t)n, "Content-Length: %lld\r\n\r\n", length);
        } else {
            n += snprintf(head + n, sizeof head - (size_t)n, "Transfer-Encoding: chunked\r\n\r\n");
        }
    }
    if (n <= 0 || (size_t)n >= sizeof head) return false;
    a->head_sent = true;
    return write_all(a->fd, head, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* The API                                                              */
/* ------------------------------------------------------------------ */

int httpd_req_to_sockfd(httpd_req_t *r)
{
    req_aux_t *a = aux_of(r);
    return a ? a->fd : -1;
}

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    req_aux_t *a = aux_of(r);
    if (!a || !buf) return HTTPD_SOCK_ERR_INVALID;
    size_t left = r->content_len - a->body_read;
    if (left == 0 || buf_len == 0) return 0;
    if (buf_len > left) buf_len = left;

    /* What came in with the head first. */
    size_t buffered = a->head_len - a->body_at;
    if (a->body_read < buffered) {
        size_t n = buffered - a->body_read;
        if (n > buf_len) n = buf_len;
        memcpy(buf, a->head + a->body_at + a->body_read, n);
        a->body_read += n;
        return (int)n;
    }
    for (;;) {
        ssize_t n = recv(a->fd, buf, buf_len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return HTTPD_SOCK_ERR_TIMEOUT;
        if (n <= 0) return HTTPD_SOCK_ERR_FAIL;
        a->body_read += (size_t)n;
        return (int)n;
    }
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field)
{
    req_aux_t *a = aux_of(r);
    const char *v = a ? header(a, field) : NULL;
    return v ? strlen(v) : 0;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size)
{
    req_aux_t *a = aux_of(r);
    if (!a || !val || val_size == 0) return ESP_ERR_INVALID_ARG;
    const char *v = header(a, field);
    if (!v) return ESP_ERR_NOT_FOUND;
    snprintf(val, val_size, "%s", v);
    return strlen(v) < val_size ? ESP_OK : ESP_ERR_HTTPD_RESULT_TRUNC;
}

size_t httpd_req_get_url_query_len(httpd_req_t *r)
{
    req_aux_t *a = aux_of(r);
    return a && a->query ? strlen(a->query) : 0;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len)
{
    req_aux_t *a = aux_of(r);
    if (!a || !buf || buf_len == 0) return ESP_ERR_INVALID_ARG;
    if (!a->query) return ESP_ERR_NOT_FOUND;
    snprintf(buf, buf_len, "%s", a->query);
    return strlen(a->query) < buf_len ? ESP_OK : ESP_ERR_HTTPD_RESULT_TRUNC;
}

/* As ESP-IDF's: the value as it is in the query, not decoded, and the key
 * matched in either case. */
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size)
{
    if (!qry || !key || !val || val_size == 0) return ESP_ERR_INVALID_ARG;
    size_t key_len = strlen(key);
    const char *p = qry;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        if (eq && (size_t)(eq - p) == key_len && strncasecmp(p, key, key_len) == 0) {
            size_t vlen = len - key_len - 1;
            size_t copy = vlen < val_size - 1 ? vlen : val_size - 1;
            memcpy(val, eq + 1, copy);
            val[copy] = '\0';
            return vlen < val_size ? ESP_OK : ESP_ERR_HTTPD_RESULT_TRUNC;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    req_aux_t *a = aux_of(r);
    if (!a) return ESP_ERR_HTTPD_INVALID_REQ;
    a->status = status;
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{
    req_aux_t *a = aux_of(r);
    if (!a) return ESP_ERR_HTTPD_INVALID_REQ;
    a->type = type;
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    req_aux_t *a = aux_of(r);
    if (!a) return ESP_ERR_HTTPD_INVALID_REQ;
    server_t *srv = r->handle;
    if (a->resp_count >= MAX_RESP_HDR || a->resp_count >= srv->config.max_resp_headers) {
        return ESP_ERR_HTTPD_RESP_HDR;
    }
    a->resp_names[a->resp_count] = field;
    a->resp_values[a->resp_count] = value;
    a->resp_count++;
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len)
{
    req_aux_t *a = aux_of(r);
    if (!a) return ESP_ERR_HTTPD_INVALID_REQ;
    if (a->head_sent) return ESP_ERR_HTTPD_RESP_SEND;
    size_t len = buf_len == HTTPD_RESP_USE_STRLEN ? (buf ? strlen(buf) : 0) : (size_t)buf_len;
    if (!send_head(a, (long long)len) || (len && !write_all(a->fd, buf, len))) {
        a->failed = true;
        return ESP_ERR_HTTPD_RESP_SEND;
    }
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t buf_len)
{
    req_aux_t *a = aux_of(r);
    if (!a) return ESP_ERR_HTTPD_INVALID_REQ;
    if (!a->head_sent) {
        a->chunked = true;
        if (!send_head(a, -1)) {
            a->failed = true;
            return ESP_ERR_HTTPD_RESP_SEND;
        }
    }
    size_t len = buf_len == HTTPD_RESP_USE_STRLEN ? (buf ? strlen(buf) : 0) : (size_t)buf_len;
    char size[24];
    int n = snprintf(size, sizeof size, "%zx\r\n", len);
    bool ok = write_all(a->fd, size, (size_t)n) && (len == 0 || write_all(a->fd, buf, len)) &&
              write_all(a->fd, "\r\n", 2);
    if (!ok) {
        a->failed = true;
        return ESP_ERR_HTTPD_RESP_SEND;
    }
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg)
{
    static const char *const STATUS[] = {
        [HTTPD_500_INTERNAL_SERVER_ERROR] = "500 Internal Server Error",
        [HTTPD_501_METHOD_NOT_IMPLEMENTED] = "501 Method Not Implemented",
        [HTTPD_505_VERSION_NOT_SUPPORTED] = "505 Version Not Supported",
        [HTTPD_400_BAD_REQUEST] = "400 Bad Request",
        [HTTPD_401_UNAUTHORIZED] = "401 Unauthorized",
        [HTTPD_403_FORBIDDEN] = "403 Forbidden",
        [HTTPD_404_NOT_FOUND] = "404 Not Found",
        [HTTPD_405_METHOD_NOT_ALLOWED] = "405 Method Not Allowed",
        [HTTPD_408_REQ_TIMEOUT] = "408 Request Timeout",
        [HTTPD_411_LENGTH_REQUIRED] = "411 Length Required",
        [HTTPD_413_CONTENT_TOO_LARGE] = "413 Content Too Large",
        [HTTPD_414_URI_TOO_LONG] = "414 URI Too Long",
        [HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE] = "431 Request Header Fields Too Large",
    };
    httpd_resp_set_status(req, error < HTTPD_ERR_CODE_MAX ? STATUS[error] : STATUS[0]);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg ? msg : "", HTTPD_RESP_USE_STRLEN);
}

int httpd_send(httpd_req_t *r, const char *buf, size_t buf_len)
{
    req_aux_t *a = aux_of(r);
    if (!a || !buf) return HTTPD_SOCK_ERR_INVALID;
    a->head_sent = true;
    for (;;) {
        ssize_t n = send(a->fd, buf, buf_len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return HTTPD_SOCK_ERR_TIMEOUT;
        if (n < 0) return HTTPD_SOCK_ERR_FAIL;
        return (int)n;
    }
}

/* ------------------------------------------------------------------ */
/* Answering                                                            */
/* ------------------------------------------------------------------ */

static void simple_answer(int fd, const char *status, const char *body)
{
    char out[512];
    int n = snprintf(out, sizeof out,
                     "HTTP/1.1 %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n%s", status, strlen(body), body);
    write_all(fd, out, (size_t)n);
}

static void answer(server_t *srv, int fd)
{
    req_aux_t *a = calloc(1, sizeof *a);
    httpd_req_t *req = calloc(1, sizeof *req);
    if (!a || !req) goto out;
    a->fd = fd;
    if (!read_head(a)) {
        simple_answer(fd, "431 Request Header Fields Too Large", "Header too large");
        goto out;
    }

    /* "METHOD /uri?query HTTP/1.1" */
    char *line_end = strstr(a->head, "\r\n");
    *line_end = '\0';
    char *method = a->head;
    char *uri = strchr(method, ' ');
    char *version = uri ? strchr(uri + 1, ' ') : NULL;
    if (!uri || !version) {
        simple_answer(fd, "400 Bad Request", "Bad request line");
        goto out;
    }
    *uri++ = '\0';
    *version = '\0';
    if (strlen(uri) > CONFIG_HTTPD_MAX_URI_LEN) {
        simple_answer(fd, "414 URI Too Long", "URI too long");
        goto out;
    }
    a->head[a->body_at - 2] = '\0';   /* end the header block */
    parse_headers(a, line_end + 2);

    req->handle = srv;
    req->aux = a;
    req->method = method_of(method);
    char *path = (char *)req->uri;
    snprintf(path, sizeof req->uri, "%s", uri);
    char *q = strchr(path, '?');
    if (q) {
        *q = '\0';
        a->query = q + 1;
    }
    const char *length = header(a, "Content-Length");
    req->content_len = length ? (size_t)strtoull(length, NULL, 10) : 0;

    const httpd_uri_t *route = NULL;
    bool uri_known = false;
    for (int i = 0; i < srv->route_count; i++) {
        if (strcmp(srv->routes[i].uri, path) != 0) continue;
        uri_known = true;
        if ((int)srv->routes[i].method == req->method) {
            route = &srv->routes[i];
            break;
        }
    }
    if (!route) {
        simple_answer(fd, uri_known ? "405 Method Not Allowed" : "404 Not Found",
                      uri_known ? "Request method for this URI is not handled by server"
                                : "This URI does not exist");
        goto out;
    }
    req->user_ctx = route->user_ctx;
    esp_err_t err = route->handler(req);
    if (err != ESP_OK && !a->head_sent) {
        simple_answer(fd, "500 Internal Server Error", "Server has encountered an unexpected error");
    }

out:
    free(req);
    free(a);
}

static void *serve(void *arg)
{
    server_t *srv = arg;
    while (!srv->stop) {
        struct pollfd p = {.fd = srv->listen_fd, .events = POLLIN};
        if (poll(&p, 1, 100) <= 0) continue;
        int fd = accept(srv->listen_fd, NULL, NULL);
        if (fd < 0) continue;

        struct timeval tv = {.tv_sec = srv->config.recv_wait_timeout};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        tv.tv_sec = srv->config.send_wait_timeout;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

        if (srv->config.open_fn && srv->config.open_fn(srv, fd) != ESP_OK) {
            close(fd);
            continue;
        }
        answer(srv, fd);
        if (srv->config.close_fn) srv->config.close_fn(srv, fd);
        shutdown(fd, SHUT_WR);
        close(fd);
    }
    return NULL;
}

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    if (!handle || !config) return ESP_ERR_INVALID_ARG;
    *handle = NULL;

    uint16_t port = config->server_port < 1024 ? 8080 : config->server_port;
    const char *env = getenv("THISTLE_SIM_HTTPD_PORT");
    if (env && env[0]) port = (uint16_t)atoi(env);

    server_t *srv = calloc(1, sizeof *srv);
    if (!srv) return ESP_ERR_HTTPD_ALLOC_MEM;
    srv->config = *config;
    srv->routes = calloc(config->max_uri_handlers ? config->max_uri_handlers : 1, sizeof *srv->routes);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (fd < 0 || !srv->routes || bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(fd, config->backlog_conn ? config->backlog_conn : 5) != 0) {
        fprintf(stderr, "[sim_httpd] cannot listen on 127.0.0.1:%u: %s\n", port, strerror(errno));
        if (fd >= 0) close(fd);
        free(srv->routes);
        free(srv);
        return ESP_ERR_HTTPD_TASK;
    }
    socklen_t len = sizeof addr;
    getsockname(fd, (struct sockaddr *)&addr, &len);
    srv->listen_fd = fd;
    srv->port = ntohs(addr.sin_port);

    if (pthread_create(&srv->thread, NULL, serve, srv) != 0) {
        close(fd);
        free(srv->routes);
        free(srv);
        return ESP_ERR_HTTPD_TASK;
    }
    printf("[sim_httpd] listening on http://127.0.0.1:%u\n", srv->port);
    fflush(stdout);
    *handle = srv;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t handle)
{
    server_t *srv = handle;
    if (!srv) return ESP_ERR_INVALID_ARG;
    srv->stop = true;
    pthread_join(srv->thread, NULL);
    close(srv->listen_fd);
    printf("[sim_httpd] stopped listening on port %u\n", srv->port);
    fflush(stdout);
    free(srv->routes);
    free(srv);
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    server_t *srv = handle;
    if (!srv || !uri_handler) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < srv->route_count; i++) {
        if (strcmp(srv->routes[i].uri, uri_handler->uri) == 0 &&
            srv->routes[i].method == uri_handler->method) {
            return ESP_ERR_HTTPD_HANDLER_EXISTS;
        }
    }
    if (srv->route_count >= srv->config.max_uri_handlers) return ESP_ERR_HTTPD_HANDLERS_FULL;
    srv->routes[srv->route_count++] = *uri_handler;
    return ESP_OK;
}

uint16_t sim_httpd_port(httpd_handle_t handle)
{
    server_t *srv = handle;
    return srv ? srv->port : 0;
}

#endif /* WASM_BUILD */
