/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — a WebSocket connection on a socket, for the Hermes app.
 *
 * hermes_ws.c's framing on a BSD socket, which is lwIP's on the device (a
 * tailnet address goes through MicroLink's netif) and the host's in the
 * simulator. One task owns a connection: nothing here takes a lock.
 */
#pragma once

#include "hermes/hermes_ws.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    HERMES_WS_OPENED,
    HERMES_WS_NO_HOST,        /* the name resolves to nothing */
    HERMES_WS_REFUSED,        /* the host answered, and nothing listens on
                                 the port */
    HERMES_WS_TIMED_OUT,      /* nothing answered: the host is off, or the
                                 tailnet's policy drops the packets */
    HERMES_WS_UNREACHABLE,    /* no route to it */
    HERMES_WS_NOT_WEBSOCKET,  /* something answered, but not a WebSocket */
    HERMES_WS_CANCELLED,
    HERMES_WS_FAILED,         /* anything else */
} hermes_ws_open_t;

typedef struct {
    int fd;
    hermes_ws_decoder_t dec;
    uint8_t in[1536];
    size_t in_len;
    size_t in_at;
    int http_status;          /* the answer to the upgrade */
    char error[96];           /* why the last call failed */
} hermes_ws_conn_t;

/* Resolves, connects and upgrades within timeout_ms, giving up as soon as
 * *cancel is true. Messages are put together in msg (msg_cap bytes) and
 * read from there after each TEXT or BINARY event. */
hermes_ws_open_t hermes_ws_conn_open(hermes_ws_conn_t *c, const hermes_ws_url_t *url,
                                     uint8_t *msg, size_t msg_cap, int timeout_ms,
                                     const volatile bool *cancel);

/* Waits up to timeout_ms for a message, a close or an error, answering
 * pings meanwhile. NOTHING if none came. *heard is set to true when any
 * frame arrived, a ping included. ERROR and CLOSE leave the connection to
 * be closed. */
hermes_ws_event_t hermes_ws_conn_poll(hermes_ws_conn_t *c, int timeout_ms, bool *heard);

/* Sends one text message, waiting up to timeout_ms for room. */
bool hermes_ws_conn_send_text(hermes_ws_conn_t *c, const char *text, size_t len, int timeout_ms);

/* Says goodbye with `code` (0: without one) and closes the socket. */
void hermes_ws_conn_close(hermes_ws_conn_t *c, uint16_t code);
