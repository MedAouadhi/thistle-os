/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — a WebSocket client's framing (RFC 6455), for the Hermes app.
 *
 * Only what a client of the Hermes Device Bridge needs: ws:// URLs, the
 * opening handshake, masked frames out, and frames in, whole or in pieces,
 * put back together into messages. There is no I/O here, so all of it is
 * tested on the host (apps/hermes/tests/); hermes_ws_conn.c puts it on a
 * socket.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HERMES_WS_HOST_MAX 64
#define HERMES_WS_PATH_MAX 64
/* The bridge's port, taken for a URL that names none: what a person types
 * is "homelab", meaning the bridge, not a web server. */
#define HERMES_WS_DEFAULT_PORT 8765
/* "ws://" + host + ":65535" + path, and a NUL. */
#define HERMES_WS_URL_MAX (5 + HERMES_WS_HOST_MAX + 6 + HERMES_WS_PATH_MAX + 1)

typedef struct {
    char host[HERMES_WS_HOST_MAX];
    uint16_t port;
    char path[HERMES_WS_PATH_MAX];   /* "/" when the URL has none */
} hermes_ws_url_t;

/* Reads "ws://host[:port][/path]", or "host[:port][/path]" with ws://
 * understood. NULL on success; otherwise why not, as a sentence. */
const char *hermes_ws_parse_url(const char *text, hermes_ws_url_t *out);

/* "ws://host:port", and the path unless it is "/". */
void hermes_ws_format_url(const hermes_ws_url_t *url, char *out, size_t cap);

/* ------------------------------------------------------------------ */
/* The opening handshake                                                */
/* ------------------------------------------------------------------ */

/* The Sec-WebSocket-Key for 16 random bytes: their base64. */
void hermes_ws_make_key(const uint8_t nonce[16], char key[25]);

/* What the server must answer for that key: base64(SHA-1(key + RFC 6455's
 * GUID)). */
void hermes_ws_accept(const char *key, char accept[29]);

/* The GET that asks for the upgrade. Returns its length, or -1 if it does
 * not fit. */
int hermes_ws_request(char *out, size_t cap, const hermes_ws_url_t *url, const char *key);

typedef enum {
    HERMES_WS_HEAD_INCOMPLETE,  /* no blank line yet */
    HERMES_WS_HEAD_OK,          /* 101, upgraded, with the right accept */
    HERMES_WS_HEAD_REFUSED,     /* another status, in *status */
    HERMES_WS_HEAD_BAD,         /* not an HTTP answer, or a 101 that is not
                                   a WebSocket's answer to this key */
} hermes_ws_head_t;

/* Reads the answer to the request. On anything but INCOMPLETE, *head_len is
 * how many bytes the head took, blank line included: bytes after it are
 * already frames. */
hermes_ws_head_t hermes_ws_response(const char *data, size_t len, const char *key,
                                    int *status, size_t *head_len);

/* ------------------------------------------------------------------ */
/* Frames                                                               */
/* ------------------------------------------------------------------ */

#define HERMES_WS_OP_CONTINUATION 0x0
#define HERMES_WS_OP_TEXT         0x1
#define HERMES_WS_OP_BINARY       0x2
#define HERMES_WS_OP_CLOSE        0x8
#define HERMES_WS_OP_PING         0x9
#define HERMES_WS_OP_PONG         0xA

#define HERMES_WS_CLOSE_NORMAL        1000
#define HERMES_WS_CLOSE_GOING_AWAY    1001
#define HERMES_WS_CLOSE_PROTOCOL      1002
#define HERMES_WS_CLOSE_NO_STATUS     1005
#define HERMES_WS_CLOSE_TOO_BIG       1009

/* The header of a final frame of `len` bytes, masked with `mask`, as a
 * client must: 6 to 14 bytes, returned. */
size_t hermes_ws_frame_header(uint8_t out[14], uint8_t opcode, size_t len, const uint8_t mask[4]);

/* XORs data with the mask, as if it began `offset` bytes into the payload. */
void hermes_ws_mask(uint8_t *data, size_t len, const uint8_t mask[4], size_t offset);

typedef enum {
    HERMES_WS_NOTHING,  /* more bytes needed */
    HERMES_WS_TEXT,     /* a whole text message: msg, msg_len, NUL after it */
    HERMES_WS_BINARY,   /* a whole binary message */
    HERMES_WS_PING,     /* ctrl, ctrl_len: answer with a pong carrying them */
    HERMES_WS_PONG,
    HERMES_WS_CLOSE,    /* close_code: HERMES_WS_CLOSE_NO_STATUS if none */
    HERMES_WS_ERROR,    /* error says what, close_code what to close with;
                           nothing more can be read */
} hermes_ws_event_t;

typedef struct {
    /* The message the last TEXT or BINARY event was for: the caller's
     * buffer, of which one byte is kept for the NUL. */
    uint8_t *msg;
    size_t msg_cap;
    size_t msg_len;
    /* The last control frame's payload. */
    uint8_t ctrl[125];
    size_t ctrl_len;
    uint16_t close_code;
    const char *error;

    /* Where the decoder is. */
    uint8_t head[14];
    uint8_t head_len;
    uint8_t head_need;
    uint8_t opcode;
    bool fin;
    bool masked;
    uint8_t mask[4];
    uint64_t left;            /* payload bytes still to come */
    uint64_t done;            /* payload bytes taken */
    uint8_t message_opcode;   /* TEXT or BINARY while one is being put
                                 together from its frames, else 0 */
    bool message_ended;       /* msg holds a message already returned */
    bool failed;
} hermes_ws_decoder_t;

void hermes_ws_decoder_init(hermes_ws_decoder_t *d, uint8_t *buf, size_t cap);

/* Takes up to `len` bytes and returns how many it took. It stops after an
 * event, so call it again with the rest; what the event points at is good
 * until then. */
size_t hermes_ws_decode(hermes_ws_decoder_t *d, const uint8_t *data, size_t len,
                        hermes_ws_event_t *event);
