/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — a WebSocket connection on a socket. See hermes_ws_conn.h.
 *
 * The socket is non-blocking from the start, and every wait is a select()
 * of at most 100 ms, so a connect, a handshake or a send that cannot finish
 * is given up within a tenth of a second of being told to stop.
 */
#include "hermes/hermes_ws_conn.h"

#include "esp_timer.h"
#ifdef SIMULATOR_BUILD
#include "esp_system.h"
#else
#include "esp_random.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#if __has_include(<netinet/tcp.h>)
#include <netinet/tcp.h>
#endif
#if __has_include(<netinet/in.h>)
#include <netinet/in.h>
#endif

#define SLICE_MS 100

/* A send to a socket the far end has closed raises SIGPIPE on a host, which
 * ends the simulator; lwIP has no signals. */
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool cancelled(const volatile bool *cancel)
{
    return cancel && *cancel;
}

/* Waits for the socket to be readable or writable, up to ms. 1 when it is,
 * 0 on a timeout, -1 on an error. */
static int wait_for(int fd, bool write, int ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    int n = select(fd + 1, write ? NULL : &set, write ? &set : NULL, NULL, &tv);
    if (n < 0 && errno == EINTR) return 0;
    return n;
}

static bool send_all(hermes_ws_conn_t *c, const void *data, size_t len, int64_t deadline,
                     const volatile bool *cancel)
{
    const uint8_t *p = data;
    while (len > 0) {
        ssize_t n = send(c->fd, p, len, SEND_FLAGS);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            snprintf(c->error, sizeof(c->error), "the connection broke (errno %d)", errno);
            return false;
        }
        int64_t left = deadline - now_ms();
        if (left <= 0 || cancelled(cancel)) {
            snprintf(c->error, sizeof(c->error), "the bridge took nothing for too long");
            return false;
        }
        if (wait_for(c->fd, true, left < SLICE_MS ? (int)left : SLICE_MS) < 0) {
            snprintf(c->error, sizeof(c->error), "the connection broke (errno %d)", errno);
            return false;
        }
    }
    return true;
}

static hermes_ws_open_t from_errno(hermes_ws_conn_t *c, int err)
{
    snprintf(c->error, sizeof(c->error), "%s (errno %d)", strerror(err), err);
    switch (err) {
    case ECONNREFUSED:
    case ECONNRESET:
        return HERMES_WS_REFUSED;
    case ETIMEDOUT:
        return HERMES_WS_TIMED_OUT;
    case EHOSTUNREACH:
    case ENETUNREACH:
        return HERMES_WS_UNREACHABLE;
    default:
        return HERMES_WS_FAILED;
    }
}

static hermes_ws_open_t connect_tcp(hermes_ws_conn_t *c, const hermes_ws_url_t *url,
                                    int64_t deadline, const volatile bool *cancel)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *found = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)url->port);
    int rc = getaddrinfo(url->host, port, &hints, &found);
    if (rc != 0 || !found) {
        snprintf(c->error, sizeof(c->error), "%s is not a name the device knows", url->host);
        if (found) freeaddrinfo(found);
        return HERMES_WS_NO_HOST;
    }

    c->fd = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    if (c->fd < 0) {
        freeaddrinfo(found);
        snprintf(c->error, sizeof(c->error), "no socket (errno %d)", errno);
        return HERMES_WS_FAILED;
    }
    int flags = fcntl(c->fd, F_GETFL, 0);
    fcntl(c->fd, F_SETFL, flags | O_NONBLOCK);
#ifdef TCP_NODELAY
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif

    rc = connect(c->fd, found->ai_addr, found->ai_addrlen);
    int err = errno;
    freeaddrinfo(found);
    if (rc == 0) return HERMES_WS_OPENED;
    if (err != EINPROGRESS && err != EAGAIN) return from_errno(c, err);

    for (;;) {
        if (cancelled(cancel)) return HERMES_WS_CANCELLED;
        int64_t left = deadline - now_ms();
        if (left <= 0) {
            snprintf(c->error, sizeof(c->error), "nothing answered at %s:%u", url->host,
                     (unsigned)url->port);
            return HERMES_WS_TIMED_OUT;
        }
        int ready = wait_for(c->fd, true, left < SLICE_MS ? (int)left : SLICE_MS);
        if (ready < 0) return from_errno(c, errno);
        if (ready == 0) continue;
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) < 0) {
            return from_errno(c, errno);
        }
        return so_error == 0 ? HERMES_WS_OPENED : from_errno(c, so_error);
    }
}

static hermes_ws_open_t upgrade(hermes_ws_conn_t *c, const hermes_ws_url_t *url,
                                int64_t deadline, const volatile bool *cancel)
{
    uint8_t nonce[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = esp_random();
        memcpy(nonce + i, &r, 4);
    }
    char key[25];
    hermes_ws_make_key(nonce, key);

    char request[256 + HERMES_WS_HOST_MAX + HERMES_WS_PATH_MAX];
    int n = hermes_ws_request(request, sizeof(request), url, key);
    if (n < 0) {
        snprintf(c->error, sizeof(c->error), "the address is too long");
        return HERMES_WS_FAILED;
    }
    if (!send_all(c, request, (size_t)n, deadline, cancel)) {
        return cancelled(cancel) ? HERMES_WS_CANCELLED : HERMES_WS_FAILED;
    }

    /* The answer's head; what follows the blank line is already frames. */
    c->in_len = 0;
    for (;;) {
        int status = 0;
        size_t head_len = 0;
        hermes_ws_head_t head = hermes_ws_response((const char *)c->in, c->in_len, key,
                                                   &status, &head_len);
        c->http_status = status;
        if (head == HERMES_WS_HEAD_OK) {
            c->in_at = head_len;
            return HERMES_WS_OPENED;
        }
        if (head == HERMES_WS_HEAD_REFUSED) {
            snprintf(c->error, sizeof(c->error), "the server answered HTTP %d, not a WebSocket",
                     status);
            return HERMES_WS_NOT_WEBSOCKET;
        }
        if (head == HERMES_WS_HEAD_BAD) {
            snprintf(c->error, sizeof(c->error), "the server's answer was not a WebSocket's");
            return HERMES_WS_NOT_WEBSOCKET;
        }
        if (c->in_len == sizeof(c->in)) {
            snprintf(c->error, sizeof(c->error), "the server's answer was too long");
            return HERMES_WS_NOT_WEBSOCKET;
        }
        if (cancelled(cancel)) return HERMES_WS_CANCELLED;
        int64_t left = deadline - now_ms();
        if (left <= 0) {
            snprintf(c->error, sizeof(c->error), "the server did not answer the upgrade");
            return HERMES_WS_TIMED_OUT;
        }
        int ready = wait_for(c->fd, false, left < SLICE_MS ? (int)left : SLICE_MS);
        if (ready < 0) return from_errno(c, errno);
        if (ready == 0) continue;
        ssize_t got = recv(c->fd, c->in + c->in_len, sizeof(c->in) - c->in_len, 0);
        if (got == 0) {
            snprintf(c->error, sizeof(c->error), "the server closed the connection");
            return HERMES_WS_NOT_WEBSOCKET;
        }
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return from_errno(c, errno);
        }
        c->in_len += (size_t)got;
    }
}

hermes_ws_open_t hermes_ws_conn_open(hermes_ws_conn_t *c, const hermes_ws_url_t *url,
                                     uint8_t *msg, size_t msg_cap, int timeout_ms,
                                     const volatile bool *cancel)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    hermes_ws_decoder_init(&c->dec, msg, msg_cap);
    int64_t deadline = now_ms() + timeout_ms;

    hermes_ws_open_t result = connect_tcp(c, url, deadline, cancel);
    if (result == HERMES_WS_OPENED) result = upgrade(c, url, deadline, cancel);
    if (result != HERMES_WS_OPENED && c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    return result;
}

static bool send_frame(hermes_ws_conn_t *c, uint8_t opcode, const void *payload, size_t len,
                       int timeout_ms)
{
    if (c->fd < 0) return false;
    int64_t deadline = now_ms() + timeout_ms;
    uint32_t r = esp_random();
    uint8_t mask[4];
    memcpy(mask, &r, 4);

    uint8_t chunk[256];
    size_t head = hermes_ws_frame_header(chunk, opcode, len, mask);
    if (!send_all(c, chunk, head, deadline, NULL)) return false;
    const uint8_t *p = payload;
    for (size_t at = 0; at < len;) {
        size_t n = len - at < sizeof(chunk) ? len - at : sizeof(chunk);
        memcpy(chunk, p + at, n);
        hermes_ws_mask(chunk, n, mask, at);
        if (!send_all(c, chunk, n, deadline, NULL)) return false;
        at += n;
    }
    return true;
}

bool hermes_ws_conn_send_text(hermes_ws_conn_t *c, const char *text, size_t len, int timeout_ms)
{
    return send_frame(c, HERMES_WS_OP_TEXT, text, len, timeout_ms);
}

hermes_ws_event_t hermes_ws_conn_poll(hermes_ws_conn_t *c, int timeout_ms, bool *heard)
{
    if (c->fd < 0) return HERMES_WS_ERROR;
    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        while (c->in_at < c->in_len) {
            hermes_ws_event_t event;
            c->in_at += hermes_ws_decode(&c->dec, c->in + c->in_at, c->in_len - c->in_at, &event);
            if (event == HERMES_WS_NOTHING) continue;
            *heard = true;
            if (event == HERMES_WS_PING) {
                if (!send_frame(c, HERMES_WS_OP_PONG, c->dec.ctrl, c->dec.ctrl_len, 2000)) {
                    return HERMES_WS_ERROR;
                }
                continue;
            }
            if (event == HERMES_WS_PONG) continue;
            if (event == HERMES_WS_ERROR) {
                snprintf(c->error, sizeof(c->error), "%s", c->dec.error);
            }
            return event;
        }
        c->in_at = c->in_len = 0;

        int64_t left = deadline - now_ms();
        if (left <= 0) return HERMES_WS_NOTHING;
        int ready = wait_for(c->fd, false, (int)left);
        if (ready < 0) {
            snprintf(c->error, sizeof(c->error), "the connection broke (errno %d)", errno);
            return HERMES_WS_ERROR;
        }
        if (ready == 0) return HERMES_WS_NOTHING;
        ssize_t got = recv(c->fd, c->in, sizeof(c->in), 0);
        if (got == 0) {
            snprintf(c->error, sizeof(c->error), "the bridge closed the connection");
            return HERMES_WS_ERROR;
        }
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            snprintf(c->error, sizeof(c->error), "the connection broke (errno %d)", errno);
            return HERMES_WS_ERROR;
        }
        c->in_len = (size_t)got;
    }
}

void hermes_ws_conn_close(hermes_ws_conn_t *c, uint16_t code)
{
    if (c->fd < 0) return;
    if (code) {
        uint8_t payload[2] = { (uint8_t)(code >> 8), (uint8_t)code };
        send_frame(c, HERMES_WS_OP_CLOSE, payload, sizeof(payload), 500);
    }
    shutdown(c->fd, SHUT_RDWR);
    close(c->fd);
    c->fd = -1;
}
