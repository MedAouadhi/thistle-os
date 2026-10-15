/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — File Transfer: the web server.
 *
 * The page is web/index.html; it talks to the API below in JSON. Every
 * path comes in a query string, as `path`, and goes through
 * xfer_path_parse() before anything is done with it.
 *
 *   GET    /                         the page
 *   POST   /api/login                body: the password; sets the cookie
 *   POST   /api/logout
 *   GET    /api/info                 the roots, and how full they are
 *   GET    /api/list?path=           what is in a folder
 *   GET    /api/file?path=[&download=1]
 *   PUT    /api/file?path=[&overwrite=1]   body: the file
 *   DELETE /api/file?path=
 *   POST   /api/mkdir?path=
 *   POST   /api/rename?path=&name=
 *
 * Everything but the page and the login needs the session cookie, or the
 * password as Basic authentication. A request that changes something and
 * comes with the cookie must also carry X-Thistle-Request, which a page on
 * another site cannot send without asking first; the server answers no
 * such asking (CORS preflight), so cookies cannot be ridden from elsewhere.
 * 401s carry no WWW-Authenticate, so a browser never offers Basic itself.
 */
#include "filexfer/xfer_server.h"
#include "filexfer/xfer_fs.h"
#include "filexfer/xfer_path.h"
#include "filexfer/xfer_web.h"

#include "hal/board.h"
#include "hal/network.h"
#include "hal/sdcard_path.h"
#include "thistle/board_config.h"
#include "thistle/net_manager.h"
#include "thistle/power_manager.h"

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mdns.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>

#ifdef SIMULATOR_BUILD
#include <sys/statvfs.h>
#else
#include "esp_spiffs.h"
#endif

static const char *TAG = "filexfer";

extern int thistle_crypto_random(uint8_t *buf, size_t len);

#define PORT           80
#define IO_BUF         (8 * 1024)
#define QUERY_MAX      (CONFIG_HTTPD_MAX_URI_LEN + 1)
#define LOGIN_MAX      64
#define COOKIE         "thistle_xfer"
#define CHANGE_HEADER  "X-Thistle-Request"
/* SPIFFS slows to a crawl, and can fail to collect garbage, when full. */
#define SPIFFS_SPARE   (32 * 1024)
#define RECV_RETRIES   3

/* Scratch for the request being answered: one at a time, on the server's
 * task, so one of each does. In PSRAM: none of it is a secret or a stack. */
typedef struct {
    char query[QUERY_MAX];
    char raw[QUERY_MAX];
    char decoded[XFER_PATH_MAX];
    char name[XFER_PATH_MAX * 2];   /* a name escaped for JSON */
    char head[XFER_PATH_MAX * 4 + 512];
    char json[XFER_PATH_MAX * 6 + 128];
    char io[IO_BUF];
} work_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/* Under s_lock. The password and the sessions stay in internal RAM. */
static struct {
    bool           wanted;
    xfer_state_t   state;
    httpd_handle_t httpd;
    bool           stopping;      /* handlers give up what they are doing */
    bool           mdns;
    uint32_t       hold;
    char           ip[16];
    uint16_t       port;
    int64_t        last_use_ms;
    bool           idle_stopped;
    bool           refused_logged;
    char           activity[64];
    char           error[48];
    xfer_auth_t    auth;
} s;

static work_t *s_work;

/* ------------------------------------------------------------------ */
/* Small things                                                         */
/* ------------------------------------------------------------------ */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool random_bytes(uint8_t *buf, size_t len)
{
    if (thistle_crypto_random(buf, len) == 0) return true;
    ESP_LOGE(TAG, "no random numbers");
    return false;
}

static void new_password_locked(void)
{
    uint8_t r[XFER_PASSWORD_CHARS];
    if (random_bytes(r, sizeof r)) {
        xfer_auth_reset(&s.auth, r);
    } else {
        /* No password lets nobody in, where a guessable one would. */
        memset(&s.auth, 0, sizeof s.auth);
    }
    memset(r, 0, sizeof r);
#ifdef SIMULATOR_BUILD
    /* For simulator/tests/test_file_transfer.py, which cannot read the
     * screen. A device never writes it anywhere. */
    printf("[filexfer] password %s\n", s.auth.password);
    fflush(stdout);
#endif
}

/* What the screen says was done last. */
static void set_activity(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_activity(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&s_lock);
    vsnprintf(s.activity, sizeof s.activity, fmt, ap);
    pthread_mutex_unlock(&s_lock);
    va_end(ap);
}

static void touch(void)
{
    pthread_mutex_lock(&s_lock);
    s.last_use_ms = now_ms();
    pthread_mutex_unlock(&s_lock);
}

static bool stopping(void)
{
    pthread_mutex_lock(&s_lock);
    bool stop = s.stopping;
    pthread_mutex_unlock(&s_lock);
    return stop;
}

static void format_size(int64_t bytes, char *out, size_t cap)
{
    if (bytes < 1024) snprintf(out, cap, "%" PRId64 " B", bytes);
    else if (bytes < 1024 * 1024) snprintf(out, cap, "%.1f KB", (double)bytes / 1024);
    else snprintf(out, cap, "%.1f MB", (double)bytes / (1024 * 1024));
}

/* ------------------------------------------------------------------ */
/* The roots' space                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    bool     mounted;
    uint64_t total, free;
} space_t;

static space_t root_space(xfer_root_t root)
{
    space_t sp = {0};
#ifdef SIMULATOR_BUILD
    struct statvfs v;
    if (statvfs(xfer_fs_mount(root), &v) == 0) {
        sp.mounted = true;
        sp.total = (uint64_t)v.f_blocks * v.f_frsize;
        sp.free = (uint64_t)v.f_bavail * v.f_frsize;
    }
#else
    if (root == XFER_ROOT_SPIFFS) {
        size_t total = 0, used = 0;
        if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
            sp.mounted = true;
            sp.total = total;
            sp.free = total > used ? total - used : 0;
        }
    } else {
        const hal_registry_t *reg = hal_get_registry();
        for (int i = 0; reg && i < reg->storage_count; i++) {
            const hal_storage_driver_t *d = reg->storage[i];
            if (!d || d->type != HAL_STORAGE_TYPE_SD) continue;
            sp.mounted = d->is_mounted && d->is_mounted();
            if (sp.mounted && d->get_total_bytes && d->get_free_bytes) {
                sp.total = d->get_total_bytes();
                sp.free = d->get_free_bytes();
            }
            break;
        }
    }
#endif
    return sp;
}

/* ------------------------------------------------------------------ */
/* Answers                                                              */
/* ------------------------------------------------------------------ */

static void common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    common_headers(req);
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    char json[160];
    char esc[120];
    if (!xfer_json_escape(message, esc, sizeof esc)) esc[0] = '\0';
    snprintf(json, sizeof json, "{\"error\":\"%s\"}", esc);
    return send_json(req, status, json);
}

static esp_err_t send_errno(httpd_req_t *req, int err)
{
    switch (err) {
    case ENOENT: return send_error(req, "404 Not Found", "Not found");
    case EEXIST: return send_error(req, "409 Conflict", "Something by that name is already there");
    case ENOTEMPTY: return send_error(req, "409 Conflict", "The folder is not empty");
    case EISDIR: return send_error(req, "400 Bad Request", "That is a folder");
    case ENOTDIR: return send_error(req, "400 Bad Request", "That is a file");
    case EINVAL: return send_error(req, "400 Bad Request", "Not allowed there");
    case ENAMETOOLONG: return send_error(req, "400 Bad Request", "The name is too long");
    case ENOSPC: return send_error(req, "507 Insufficient Storage", "Not enough room");
    case ENOTSUP:
        return send_error(req, "400 Bad Request",
                          "SPIFFS has no real folders: rename the files in it instead");
    default: {
        char msg[64];
        snprintf(msg, sizeof msg, "Could not: %s", strerror(err));
        return send_error(req, "500 Internal Server Error", msg);
    }
    }
}

/* ------------------------------------------------------------------ */
/* Who is asking                                                        */
/* ------------------------------------------------------------------ */

/* Whether the request may go on; if not, it has been answered. `changes`:
 * it writes, moves or removes something. */
static bool authorised(httpd_req_t *req, bool changes)
{
    touch();
    char value[200];   /* "Basic " and the base64 of a user, ':' and LOGIN_MAX */
    bool cookie_ok = false;

    size_t len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (len > 0 && len < sizeof s_work->head &&
        httpd_req_get_hdr_value_str(req, "Cookie", s_work->head, sizeof s_work->head) == ESP_OK &&
        xfer_cookie_value(s_work->head, COOKIE, value, sizeof value)) {
        pthread_mutex_lock(&s_lock);
        cookie_ok = xfer_auth_session(&s.auth, value, now_ms());
        pthread_mutex_unlock(&s_lock);
    }
    if (cookie_ok) {
        if (changes && httpd_req_get_hdr_value_len(req, CHANGE_HEADER) == 0) {
            send_error(req, "403 Forbidden", "Missing " CHANGE_HEADER);
            return false;
        }
        return true;
    }

    char password[LOGIN_MAX + 1];
    len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len > 0 && len < sizeof value &&
        httpd_req_get_hdr_value_str(req, "Authorization", value, sizeof value) == ESP_OK &&
        xfer_basic_password(value, password, sizeof password)) {
        uint32_t retry = 0;
        pthread_mutex_lock(&s_lock);
        xfer_login_t got = xfer_auth_check(&s.auth, password, now_ms(), &retry);
        pthread_mutex_unlock(&s_lock);
        memset(password, 0, sizeof password);
        if (got == XFER_LOGIN_OK) return true;
        if (got == XFER_LOGIN_LOCKED) {
            snprintf(value, sizeof value, "%" PRIu32, retry);
            httpd_resp_set_hdr(req, "Retry-After", value);
            send_error(req, "429 Too Many Requests", "Too many wrong passwords; wait a while");
            return false;
        }
    }
    send_error(req, "401 Unauthorized", "Sign in first");
    return false;
}

/* ------------------------------------------------------------------ */
/* Query strings                                                        */
/* ------------------------------------------------------------------ */

static bool load_query(httpd_req_t *req)
{
    s_work->query[0] = '\0';
    size_t len = httpd_req_get_url_query_len(req);
    if (len == 0) return true;
    if (len >= sizeof s_work->query) return false;
    return httpd_req_get_url_query_str(req, s_work->query, sizeof s_work->query) == ESP_OK;
}

/* The query's value for `key`, decoded, into s_work->decoded. */
static bool query_value(const char *key)
{
    if (httpd_query_key_value(s_work->query, key, s_work->raw, sizeof s_work->raw) != ESP_OK) {
        return false;
    }
    return xfer_url_decode(s_work->raw, s_work->decoded, sizeof s_work->decoded);
}

static bool query_flag(const char *key)
{
    return query_value(key) && strcmp(s_work->decoded, "1") == 0;
}

/* The request's `path`, checked; if there is none fit to use, the request
 * has been answered. */
static bool query_path(httpd_req_t *req, xfer_path_t *out, bool root_ok)
{
    if (!load_query(req) || !query_value("path") || !xfer_path_parse(s_work->decoded, out)) {
        send_error(req, "400 Bad Request", "Not a path this can use");
        return false;
    }
    if (out->root == XFER_ROOT_NONE && !root_ok) {
        send_error(req, "400 Bad Request", "Pick /sdcard or /spiffs");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* The page, signing in and out                                         */
/* ------------------------------------------------------------------ */

static esp_err_t page_get(httpd_req_t *req)
{
    touch();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; script-src 'self' 'unsafe-inline'; "
                       "style-src 'self' 'unsafe-inline'; img-src 'self' blob: data:; "
                       "connect-src 'self'; frame-ancestors 'none'; form-action 'none'");
    return httpd_resp_send(req, xfer_index_html, (ssize_t)xfer_index_html_len);
}

static esp_err_t login_post(httpd_req_t *req)
{
    touch();
    if (httpd_req_get_hdr_value_len(req, CHANGE_HEADER) == 0) {
        return send_error(req, "403 Forbidden", "Missing " CHANGE_HEADER);
    }
    if (req->content_len > LOGIN_MAX) return send_error(req, "400 Bad Request", "That is no password");

    char typed[LOGIN_MAX + 1];
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, typed + got, req->content_len - got);
        if (n <= 0) return ESP_FAIL;
        got += (size_t)n;
    }
    typed[got] = '\0';

    uint32_t retry = 0;
    char token[XFER_TOKEN_LEN + 1];
    bool session = false;
    pthread_mutex_lock(&s_lock);
    xfer_login_t result = xfer_auth_check(&s.auth, typed, now_ms(), &retry);
    if (result == XFER_LOGIN_OK) {
        uint8_t r[XFER_TOKEN_BYTES];
        session = random_bytes(r, sizeof r);
        if (session) xfer_auth_open_session(&s.auth, r, now_ms(), token);
        memset(r, 0, sizeof r);
    }
    pthread_mutex_unlock(&s_lock);
    memset(typed, 0, sizeof typed);

    char json[96];
    if (result == XFER_LOGIN_OK && !session) {
        return send_error(req, "500 Internal Server Error", "No random numbers for a session");
    }
    if (result == XFER_LOGIN_OK) {
        static char cookie[XFER_TOKEN_LEN + 64];
        snprintf(cookie, sizeof cookie, COOKIE "=%s; Path=/; HttpOnly; SameSite=Strict", token);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        set_activity("A browser signed in");
        esp_err_t err = send_json(req, "200 OK", "{\"ok\":true}");
        memset(cookie, 0, sizeof cookie);
        memset(token, 0, sizeof token);
        return err;
    }
    if (result == XFER_LOGIN_LOCKED) {
        char after[16];
        snprintf(after, sizeof after, "%" PRIu32, retry);
        httpd_resp_set_hdr(req, "Retry-After", after);
        snprintf(json, sizeof json,
                 "{\"error\":\"Too many wrong passwords; try again in %" PRIu32 " s\","
                 "\"retry_after\":%" PRIu32 "}", retry, retry);
        return send_json(req, "429 Too Many Requests", json);
    }
    snprintf(json, sizeof json, "{\"error\":\"Wrong password\",\"retry_after\":%" PRIu32 "}", retry);
    return send_json(req, "401 Unauthorized", json);
}

static esp_err_t logout_post(httpd_req_t *req)
{
    touch();
    char value[XFER_TOKEN_LEN + 8];
    if (httpd_req_get_hdr_value_len(req, CHANGE_HEADER) > 0 &&
        httpd_req_get_hdr_value_str(req, "Cookie", s_work->head, sizeof s_work->head) == ESP_OK &&
        xfer_cookie_value(s_work->head, COOKIE, value, sizeof value)) {
        pthread_mutex_lock(&s_lock);
        xfer_auth_close_session(&s.auth, value);
        pthread_mutex_unlock(&s_lock);
    }
    httpd_resp_set_hdr(req, "Set-Cookie", COOKIE "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return send_json(req, "200 OK", "{\"ok\":true}");
}

/* ------------------------------------------------------------------ */
/* Looking                                                              */
/* ------------------------------------------------------------------ */

static esp_err_t info_get(httpd_req_t *req)
{
    if (!authorised(req, false)) return ESP_OK;
    char board[64];
    if (!xfer_json_escape(board_config_get_name(), board, sizeof board)) board[0] = '\0';

    char *j = s_work->json;
    size_t cap = sizeof s_work->json;
    int n = snprintf(j, cap, "{\"hostname\":\"" XFER_HOSTNAME ".local\",\"board\":\"%s\",\"roots\":[",
                     board);
    for (int r = 0; r < XFER_ROOT_COUNT && n > 0 && (size_t)n < cap; r++) {
        space_t sp = root_space((xfer_root_t)r);
        n += snprintf(j + n, cap - (size_t)n,
                      "%s{\"name\":\"%s\",\"mounted\":%s,\"total\":%" PRIu64 ",\"free\":%" PRIu64 "}",
                      r ? "," : "", xfer_root_name((xfer_root_t)r), sp.mounted ? "true" : "false",
                      sp.total, sp.free);
    }
    if (n > 0 && (size_t)n < cap) snprintf(j + n, cap - (size_t)n, "]}");
    return send_json(req, "200 OK", j);
}

typedef struct {
    httpd_req_t *req;
    size_t       used;      /* of s_work->io */
    bool         first;
    bool         failed;
} listing_t;

static bool list_flush(listing_t *l)
{
    if (l->used && !l->failed &&
        httpd_resp_send_chunk(l->req, s_work->io, (ssize_t)l->used) != ESP_OK) {
        l->failed = true;
    }
    l->used = 0;
    return !l->failed;
}

static bool list_append(listing_t *l, const char *text)
{
    size_t len = strlen(text);
    if (l->used + len > sizeof s_work->io && !list_flush(l)) return false;
    memcpy(s_work->io + l->used, text, len);
    l->used += len;
    return true;
}

static bool list_entry(const xfer_entry_t *e, void *ctx)
{
    listing_t *l = ctx;
    if (!xfer_json_escape(e->name, s_work->name, sizeof s_work->name)) return true;
    snprintf(s_work->json, sizeof s_work->json,
             "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%" PRId64 ",\"mtime\":%" PRId64 "}",
             l->first ? "" : ",", s_work->name, e->dir ? "true" : "false", e->size, e->mtime);
    l->first = false;
    return list_append(l, s_work->json);
}

static esp_err_t list_get(httpd_req_t *req)
{
    if (!authorised(req, false)) return ESP_OK;
    xfer_path_t dir;
    if (!query_path(req, &dir, true)) return ESP_OK;

    if (dir.root != XFER_ROOT_NONE) {
        /* Find out before the first chunk, while an error can still be the
         * status. */
        int err = xfer_fs_list(&dir, NULL, NULL);
        if (err) return send_errno(req, err);
    }

    httpd_resp_set_type(req, "application/json");
    common_headers(req);
    listing_t l = {.req = req, .first = true};
    if (!xfer_json_escape(dir.path, s_work->decoded, sizeof s_work->decoded)) {
        s_work->decoded[0] = '\0';
    }
    snprintf(s_work->json, sizeof s_work->json, "{\"path\":\"%s\",\"entries\":[", s_work->decoded);
    list_append(&l, s_work->json);

    if (dir.root == XFER_ROOT_NONE) {
        for (int r = 0; r < XFER_ROOT_COUNT; r++) {
            if (!root_space((xfer_root_t)r).mounted) continue;
            xfer_entry_t e = {.name = xfer_root_name((xfer_root_t)r), .dir = true};
            list_entry(&e, &l);
        }
    } else {
        xfer_fs_list(&dir, list_entry, &l);
    }
    list_append(&l, "]}");
    list_flush(&l);
    if (l.failed) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Sends all of `len` bytes, as httpd_send() may not. */
static bool send_all(httpd_req_t *req, const char *buf, size_t len)
{
    while (len > 0) {
        int n = httpd_send(req, buf, len);
        if (n <= 0) return false;
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

static esp_err_t file_get(httpd_req_t *req)
{
    if (!authorised(req, false)) return ESP_OK;
    xfer_path_t p;
    if (!query_path(req, &p, false)) return ESP_OK;
    bool download = query_flag("download");

    FILE *f = NULL;
    int64_t size = 0;
    int err = xfer_fs_open_read(&p, &f, &size);
    if (err) return send_errno(req, err);

    const char *name = xfer_basename(p.path);
    char disposition[XFER_PATH_MAX * 3 + 64];
    if (!download || !xfer_content_disposition(name, disposition, sizeof disposition)) {
        snprintf(disposition, sizeof disposition, "inline");
    }

    /* Sent by hand, with the length, so that a browser shows how far a
     * download has got: httpd_resp_send_chunk() would leave it out. The
     * sandbox keeps a file that is a page from running as this site. */
    snprintf(s_work->head, sizeof s_work->head,
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %" PRId64 "\r\n"
             "Content-Disposition: %s\r\n"
             "Content-Security-Policy: sandbox; default-src 'none'; img-src 'self'; "
             "style-src 'unsafe-inline'\r\n"
             "X-Content-Type-Options: nosniff\r\n"
             "Cache-Control: no-store\r\n"
             "\r\n",
             download ? "application/octet-stream" : xfer_content_type(name), size, disposition);

    bool ok = send_all(req, s_work->head, strlen(s_work->head));
    int64_t left = size;
    while (ok && left > 0) {
        if (stopping()) {
            ok = false;
            break;
        }
        size_t want = left < IO_BUF ? (size_t)left : IO_BUF;
        size_t n = fread(s_work->io, 1, want, f);
        if (n == 0) {
            ok = false;
            break;
        }
        ok = send_all(req, s_work->io, n);
        left -= (int64_t)n;
        touch();
    }
    fclose(f);
    if (!ok) {
        ESP_LOGW(TAG, "sending %s stopped", name);
        /* The length promised was not kept: the connection has to go. */
        return ESP_FAIL;
    }
    if (download) set_activity("Sent %s", name);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Changing                                                             */
/* ------------------------------------------------------------------ */

static esp_err_t file_put(httpd_req_t *req)
{
    if (!authorised(req, true)) return ESP_OK;
    xfer_path_t p;
    if (!query_path(req, &p, false)) return ESP_OK;
    bool overwrite = query_flag("overwrite");
    const char *name = xfer_basename(p.path);

    space_t sp = root_space(p.root);
    if (!sp.mounted) return send_error(req, "404 Not Found", "That is not mounted");
    uint64_t spare = p.root == XFER_ROOT_SPIFFS ? SPIFFS_SPARE : 0;
    if (sp.total && (uint64_t)req->content_len + spare > sp.free) {
        char need[16], have[16], msg[80];
        format_size((int64_t)req->content_len, need, sizeof need);
        format_size((int64_t)(sp.free > spare ? sp.free - spare : 0), have, sizeof have);
        snprintf(msg, sizeof msg, "Not enough room: %s to write, %s free", need, have);
        return send_error(req, "507 Insufficient Storage", msg);
    }

    xfer_upload_t up;
    struct stat st;
    char real[XFER_PATH_MAX + 128];
    bool existed = xfer_fs_real(&p, real, sizeof real) && stat(real, &st) == 0;
    int err = xfer_fs_upload_begin(&up, &p, overwrite);
    if (err) return send_errno(req, err);

    size_t left = req->content_len;
    int timeouts = 0;
    while (left > 0) {
        if (stopping()) {
            xfer_fs_upload_abort(&up);
            return ESP_FAIL;
        }
        int n = httpd_req_recv(req, s_work->io, left < IO_BUF ? left : IO_BUF);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= RECV_RETRIES) continue;
        if (n <= 0) {
            ESP_LOGW(TAG, "%s came in short", name);
            xfer_fs_upload_abort(&up);
            return ESP_FAIL;
        }
        timeouts = 0;
        err = xfer_fs_upload_write(&up, s_work->io, (size_t)n);
        if (err) {
            xfer_fs_upload_abort(&up);
            /* The rest of the body is still coming; answering mid-way and
             * closing is all that can be done with it. */
            send_errno(req, err);
            return ESP_FAIL;
        }
        left -= (size_t)n;
        touch();
    }
    err = xfer_fs_upload_commit(&up);
    if (err) return send_errno(req, err);

    char size[16];
    format_size((int64_t)req->content_len, size, sizeof size);
    ESP_LOGI(TAG, "saved %s (%s)", p.path, size);
    set_activity("Saved %s", name);
    return send_json(req, existed ? "200 OK" : "201 Created", "{\"ok\":true}");
}

static esp_err_t file_delete(httpd_req_t *req)
{
    if (!authorised(req, true)) return ESP_OK;
    xfer_path_t p;
    if (!query_path(req, &p, false)) return ESP_OK;
    int err = xfer_fs_delete(&p);
    if (err) return send_errno(req, err);
    ESP_LOGI(TAG, "removed %s", p.path);
    set_activity("Removed %s", xfer_basename(p.path));
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t mkdir_post(httpd_req_t *req)
{
    if (!authorised(req, true)) return ESP_OK;
    xfer_path_t p;
    if (!query_path(req, &p, false)) return ESP_OK;
    int err = xfer_fs_mkdir(&p);
    if (err) return send_errno(req, err);
    set_activity("Made the folder %s", xfer_basename(p.path));
    return send_json(req, "201 Created", "{\"ok\":true}");
}

static esp_err_t rename_post(httpd_req_t *req)
{
    if (!authorised(req, true)) return ESP_OK;
    xfer_path_t from, to;
    if (!query_path(req, &from, false)) return ESP_OK;
    if (!query_value("name")) return send_error(req, "400 Bad Request", "No new name");
    int err = xfer_fs_rename(&from, s_work->decoded, &to);
    if (err) return send_errno(req, err);
    ESP_LOGI(TAG, "renamed %s to %s", from.path, to.path);
    set_activity("Renamed to %s", xfer_basename(to.path));

    char esc[XFER_PATH_MAX * 2];
    if (!xfer_json_escape(to.path, esc, sizeof esc)) esc[0] = '\0';
    snprintf(s_work->json, sizeof s_work->json, "{\"ok\":true,\"path\":\"%s\"}", esc);
    return send_json(req, "200 OK", s_work->json);
}

/* ------------------------------------------------------------------ */
/* Only on WiFi                                                         */
/* ------------------------------------------------------------------ */

/* Closes a connection that did not come in on the WiFi address: with
 * mobile data on, the server is listening on the modem's link too. */
static esp_err_t on_open(httpd_handle_t hd, int fd)
{
    (void)hd;
    struct sockaddr_storage local;
    socklen_t len = sizeof local;
    char ip[16] = "";
    if (getsockname(fd, (struct sockaddr *)&local, &len) == 0) {
        const uint8_t *a = NULL;
        if (local.ss_family == AF_INET) {
            a = (const uint8_t *)&((struct sockaddr_in *)&local)->sin_addr;
        } else if (local.ss_family == AF_INET6) {
            /* IPv4 on a dual-stack socket, as ::ffff:a.b.c.d */
            const uint8_t *b = (const uint8_t *)&((struct sockaddr_in6 *)&local)->sin6_addr;
            static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
            if (memcmp(b, mapped, sizeof mapped) == 0) a = b + 12;
        }
        if (a) snprintf(ip, sizeof ip, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    }

    pthread_mutex_lock(&s_lock);
    bool ok = ip[0] && strcmp(ip, s.ip) == 0;
    bool log = !ok && !s.refused_logged;
    if (log) s.refused_logged = true;
    pthread_mutex_unlock(&s_lock);
    if (log) ESP_LOGW(TAG, "refused a connection to %s, which is not WiFi", ip[0] ? ip : "?");
    return ok ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/* Starting and stopping                                                */
/* ------------------------------------------------------------------ */

/* The address on WiFi, or on the simulator's host network. */
static bool lan_ip(char *out, size_t cap)
{
    const hal_net_driver_t *list[8];
    int n = net_list_transports(list, 8);
    for (int i = 0; i < n; i++) {
        const hal_net_driver_t *d = list[i];
        if (!d || (d->type != HAL_NET_WIFI && d->type != HAL_NET_HOST)) continue;
        if (!d->is_connected || !d->is_connected() || !d->get_ip) continue;
        const char *ip = d->get_ip();
        if (!ip || !ip[0] || strcmp(ip, "0.0.0.0") == 0) continue;
        snprintf(out, cap, "%s", ip);
        return true;
    }
    return false;
}

static const httpd_uri_t ROUTES[] = {
    {.uri = "/", .method = HTTP_GET, .handler = page_get},
    {.uri = "/api/login", .method = HTTP_POST, .handler = login_post},
    {.uri = "/api/logout", .method = HTTP_POST, .handler = logout_post},
    {.uri = "/api/info", .method = HTTP_GET, .handler = info_get},
    {.uri = "/api/list", .method = HTTP_GET, .handler = list_get},
    {.uri = "/api/file", .method = HTTP_GET, .handler = file_get},
    {.uri = "/api/file", .method = HTTP_PUT, .handler = file_put},
    {.uri = "/api/file", .method = HTTP_DELETE, .handler = file_delete},
    {.uri = "/api/mkdir", .method = HTTP_POST, .handler = mkdir_post},
    {.uri = "/api/rename", .method = HTTP_POST, .handler = rename_post},
};
#define ROUTE_COUNT (sizeof ROUTES / sizeof ROUTES[0])

static void set_mounts(void)
{
#ifdef SIMULATOR_BUILD
    /* SPIFFS is a host folder in the simulator, beside the SD card's. */
    const char *spiffs = getenv("THISTLE_SIM_SPIFFS");
    if (!spiffs || !spiffs[0]) spiffs = "/tmp/thistle_spiffs";
    mkdir(spiffs, 0755);
    xfer_fs_set_mount(XFER_ROOT_SDCARD, THISTLE_SDCARD, false);
    xfer_fs_set_mount(XFER_ROOT_SPIFFS, spiffs, false);
#else
    xfer_fs_set_mount(XFER_ROOT_SDCARD, THISTLE_SDCARD, false);
    xfer_fs_set_mount(XFER_ROOT_SPIFFS, "/spiffs", true);
#endif
}

/* Starts the server and mDNS on `ip`. Called without s_lock. */
static esp_err_t bring_up(const char *ip)
{
    if (!s_work) {
        s_work = heap_caps_malloc(sizeof *s_work, MALLOC_CAP_SPIRAM);
        if (!s_work) s_work = malloc(sizeof *s_work);
        if (!s_work) return ESP_ERR_NO_MEM;
    }
    set_mounts();
    /* An upload cut short by a restart leaves its temporary file. */
    for (int r = 0; r < XFER_ROOT_COUNT; r++) {
        char tmp[160];
        snprintf(tmp, sizeof tmp, "%s/" XFER_UPLOAD_TMP, xfer_fs_mount((xfer_root_t)r));
        remove(tmp);
    }

    pthread_mutex_lock(&s_lock);
    snprintf(s.ip, sizeof s.ip, "%s", ip);
    s.stopping = false;
    s.refused_logged = false;
    pthread_mutex_unlock(&s_lock);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = PORT;
    /* newlib's printf alone takes over a kilobyte of it on Xtensa. */
    cfg.stack_size = 8192;
    /* Three more go to the server itself; lwIP has ten in all. */
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = ROUTE_COUNT;
    cfg.max_resp_headers = 12;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.open_fn = on_open;

    httpd_handle_t hd = NULL;
    esp_err_t err = httpd_start(&hd, &cfg);
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < ROUTE_COUNT; i++) httpd_register_uri_handler(hd, &ROUTES[i]);

    uint16_t port = PORT;
#ifdef SIMULATOR_BUILD
    port = sim_httpd_port(hd);
#endif

    bool mdns = mdns_init() == ESP_OK;
    if (mdns) {
        mdns_hostname_set(XFER_HOSTNAME);
        mdns_instance_name_set("ThistleOS");
        mdns_service_add("ThistleOS files", "_http", "_tcp", port, NULL, 0);
    } else {
        ESP_LOGW(TAG, "mDNS would not start: only the address will do");
    }

    uint32_t hold = power_manager_hold("files are shared over WiFi");

    pthread_mutex_lock(&s_lock);
    s.httpd = hd;
    s.port = port;
    s.mdns = mdns;
    s.hold = hold;
    s.state = XFER_RUNNING;
    s.last_use_ms = now_ms();
    s.error[0] = '\0';
    pthread_mutex_unlock(&s_lock);
    ESP_LOGI(TAG, "serving http://%s:%u and http://" XFER_HOSTNAME ".local", ip, port);
    return ESP_OK;
}

/* Stops the server and mDNS. Called without s_lock: stopping waits for the
 * request being answered, which takes the lock. */
static void bring_down(void)
{
    pthread_mutex_lock(&s_lock);
    httpd_handle_t hd = s.httpd;
    bool mdns = s.mdns;
    uint32_t hold = s.hold;
    s.stopping = true;
    s.httpd = NULL;
    s.mdns = false;
    s.hold = 0;
    pthread_mutex_unlock(&s_lock);

    if (hd) {
        httpd_stop(hd);
        ESP_LOGI(TAG, "stopped serving");
    }
    if (mdns) mdns_free();
    if (hold) power_manager_release(hold);
}

void xfer_server_start(void)
{
    pthread_mutex_lock(&s_lock);
    bool was = s.wanted;
    s.wanted = true;
    s.idle_stopped = false;
    s.activity[0] = '\0';
    if (!was || !s.auth.password[0]) new_password_locked();
    if (s.state != XFER_RUNNING) s.state = XFER_NO_WIFI;
    pthread_mutex_unlock(&s_lock);
    xfer_server_poll();
}

static void switch_off(bool idle)
{
    bring_down();
    pthread_mutex_lock(&s_lock);
    s.wanted = false;
    s.state = XFER_OFF;
    s.idle_stopped = idle;
    memset(&s.auth, 0, sizeof s.auth);
    pthread_mutex_unlock(&s_lock);
}

void xfer_server_stop(void)
{
    switch_off(false);
}

void xfer_server_new_password(void)
{
    pthread_mutex_lock(&s_lock);
    if (s.wanted) {
        new_password_locked();
        s.activity[0] = '\0';
    }
    pthread_mutex_unlock(&s_lock);
}

void xfer_server_poll(void)
{
    char ip[16] = "";
    bool have_ip = lan_ip(ip, sizeof ip);

    pthread_mutex_lock(&s_lock);
    bool wanted = s.wanted;
    bool running = s.httpd != NULL;
    bool idle = running && now_ms() - s.last_use_ms > (int64_t)XFER_IDLE_STOP_S * 1000;
    bool failed = s.state == XFER_FAILED;
    bool moved = running && have_ip && strcmp(ip, s.ip) != 0;
    if (moved) {
        ESP_LOGI(TAG, "WiFi address is now %s", ip);
        snprintf(s.ip, sizeof s.ip, "%s", ip);
    }
    pthread_mutex_unlock(&s_lock);

    if (!wanted) return;
    if (idle) {
        ESP_LOGI(TAG, "unused for %d minutes: stopping", XFER_IDLE_STOP_S / 60);
        switch_off(true);
        return;
    }
    if (running && !have_ip) {
        ESP_LOGI(TAG, "WiFi is gone: waiting for it");
        bring_down();
        pthread_mutex_lock(&s_lock);
        s.state = XFER_NO_WIFI;
        pthread_mutex_unlock(&s_lock);
        return;
    }
    if (!running && have_ip && !failed) {
        esp_err_t err = bring_up(ip);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "the server would not start: %s", esp_err_to_name(err));
            pthread_mutex_lock(&s_lock);
            s.state = XFER_FAILED;
            snprintf(s.error, sizeof s.error, "The server would not start (%s)",
                     esp_err_to_name(err));
            pthread_mutex_unlock(&s_lock);
        }
    }
}

void xfer_server_status(xfer_status_t *out)
{
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&s_lock);
    out->state = s.state;
    if (s.wanted) memcpy(out->password, s.auth.password, sizeof out->password);
    if (s.state == XFER_RUNNING) {
        memcpy(out->ip, s.ip, sizeof out->ip);
        out->port = s.port;
        out->mdns = s.mdns;
    }
    out->sessions = xfer_auth_session_count(&s.auth);
    out->failed_logins = s.auth.failures_total;
    out->locked = xfer_auth_locked(&s.auth, now_ms());
    out->idle_stopped = s.idle_stopped;
    memcpy(out->activity, s.activity, sizeof out->activity);
    memcpy(out->error, s.error, sizeof out->error);
    pthread_mutex_unlock(&s_lock);
}
