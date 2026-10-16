/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — WebSocket framing for the Hermes app. See hermes_ws.h.
 */
#include "hermes/hermes_ws.h"

#include "podcasts/pod_sha1.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* ------------------------------------------------------------------ */
/* URLs                                                                 */
/* ------------------------------------------------------------------ */

const char *hermes_ws_parse_url(const char *text, hermes_ws_url_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!text) return "No address.";
    while (*text == ' ' || *text == '\t') text++;
    size_t n = strlen(text);
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r' ||
                     text[n - 1] == '\n')) {
        n--;
    }
    if (n == 0) return "No address.";

    const char *p = text, *end = text + n;
    if (n >= 6 && strncasecmp(p, "wss://", 6) == 0) {
        return "wss:// is not supported: the bridge is reached as ws:// inside the tailnet.";
    }
    if (n >= 5 && strncasecmp(p, "ws://", 5) == 0) {
        p += 5;
    } else {
        const char *scheme = strstr(text, "://");
        if (scheme && scheme < end) return "The address must start ws://.";
    }

    const char *host = p;
    while (p < end && *p != ':' && *p != '/') p++;
    size_t host_len = (size_t)(p - host);
    if (host_len == 0) return "The address has no host.";
    if (host_len >= sizeof(out->host)) return "The host name is too long.";
    for (size_t i = 0; i < host_len; i++) {
        unsigned char c = (unsigned char)host[i];
        if (!(isalnum(c) || c == '-' || c == '.' || c == '_')) {
            return "The host may hold letters, digits, '-', '_' and '.' only.";
        }
    }
    memcpy(out->host, host, host_len);

    out->port = HERMES_WS_DEFAULT_PORT;
    if (p < end && *p == ':') {
        p++;
        unsigned long port = 0;
        const char *digits = p;
        while (p < end && isdigit((unsigned char)*p)) {
            port = port * 10 + (unsigned long)(*p - '0');
            if (port > 65535) return "The port must be 1 to 65535.";
            p++;
        }
        if (p == digits || port == 0) return "The port must be 1 to 65535.";
        out->port = (uint16_t)port;
    }

    if (p < end) {
        if (*p != '/') return "The address is not a ws:// URL.";
        size_t path_len = (size_t)(end - p);
        if (path_len >= sizeof(out->path)) return "The path is too long.";
        for (size_t i = 0; i < path_len; i++) {
            unsigned char c = (unsigned char)p[i];
            if (c <= ' ' || c >= 0x7F) return "The path may not hold spaces.";
        }
        memcpy(out->path, p, path_len);
    } else {
        strcpy(out->path, "/");
    }
    return NULL;
}

void hermes_ws_format_url(const hermes_ws_url_t *url, char *out, size_t cap)
{
    snprintf(out, cap, "ws://%s:%u%s", url->host, (unsigned)url->port,
             strcmp(url->path, "/") == 0 ? "" : url->path);
}

/* ------------------------------------------------------------------ */
/* The handshake                                                        */
/* ------------------------------------------------------------------ */

static void base64(const uint8_t *in, size_t len, char *out)
{
    static const char ALPHABET[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = ALPHABET[(v >> 18) & 63];
        out[o++] = ALPHABET[(v >> 12) & 63];
        out[o++] = i + 1 < len ? ALPHABET[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? ALPHABET[v & 63] : '=';
    }
    out[o] = '\0';
}

void hermes_ws_make_key(const uint8_t nonce[16], char key[25])
{
    base64(nonce, 16, key);
}

void hermes_ws_accept(const char *key, char accept[29])
{
    char joined[64 + sizeof(GUID)];
    snprintf(joined, sizeof(joined), "%s%s", key, GUID);
    uint8_t digest[20];
    pod_sha1(joined, strlen(joined), digest);
    base64(digest, sizeof(digest), accept);
}

int hermes_ws_request(char *out, size_t cap, const hermes_ws_url_t *url, const char *key)
{
    int n = snprintf(out, cap,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%u\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "\r\n",
                     url->path, url->host, (unsigned)url->port, key);
    return n > 0 && (size_t)n < cap ? n : -1;
}

/* Whether a header's value, as a comma-separated list, holds `token`. */
static bool has_token(const char *value, size_t len, const char *token)
{
    size_t tlen = strlen(token);
    size_t i = 0;
    while (i < len) {
        while (i < len && (value[i] == ' ' || value[i] == '\t' || value[i] == ',')) i++;
        size_t start = i;
        while (i < len && value[i] != ',') i++;
        size_t stop = i;
        while (stop > start && (value[stop - 1] == ' ' || value[stop - 1] == '\t')) stop--;
        if (stop - start == tlen && strncasecmp(value + start, token, tlen) == 0) return true;
    }
    return false;
}

hermes_ws_head_t hermes_ws_response(const char *data, size_t len, const char *key,
                                    int *status, size_t *head_len)
{
    *status = 0;
    *head_len = 0;
    const char *end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            end = data + i + 4;
            break;
        }
    }
    if (!end) return HERMES_WS_HEAD_INCOMPLETE;
    *head_len = (size_t)(end - data);

    /* "HTTP/1.1 101 Switching Protocols" */
    if (len < 12 || strncmp(data, "HTTP/1.", 7) != 0 || data[8] != ' ' ||
        !isdigit((unsigned char)data[9]) || !isdigit((unsigned char)data[10]) ||
        !isdigit((unsigned char)data[11])) {
        return HERMES_WS_HEAD_BAD;
    }
    *status = (data[9] - '0') * 100 + (data[10] - '0') * 10 + (data[11] - '0');
    if (*status != 101) return HERMES_WS_HEAD_REFUSED;

    char expected[29];
    hermes_ws_accept(key, expected);
    bool upgraded = false, connection = false, accepted = false;

    const char *line = memchr(data, '\n', (size_t)(end - data));
    while (line && line + 1 < end) {
        line++;
        const char *eol = memchr(line, '\n', (size_t)(end - line));
        if (!eol) break;
        size_t line_len = (size_t)(eol - line);
        if (line_len > 0 && line[line_len - 1] == '\r') line_len--;
        const char *colon = memchr(line, ':', line_len);
        if (colon) {
            size_t name_len = (size_t)(colon - line);
            const char *value = colon + 1;
            size_t value_len = line_len - name_len - 1;
            while (value_len > 0 && (*value == ' ' || *value == '\t')) {
                value++;
                value_len--;
            }
            while (value_len > 0 && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t')) {
                value_len--;
            }
            if (name_len == 7 && strncasecmp(line, "Upgrade", 7) == 0) {
                upgraded = has_token(value, value_len, "websocket");
            } else if (name_len == 10 && strncasecmp(line, "Connection", 10) == 0) {
                connection = has_token(value, value_len, "upgrade");
            } else if (name_len == 20 && strncasecmp(line, "Sec-WebSocket-Accept", 20) == 0) {
                accepted = value_len == 28 && memcmp(value, expected, 28) == 0;
            }
        }
        line = eol;
    }
    return upgraded && connection && accepted ? HERMES_WS_HEAD_OK : HERMES_WS_HEAD_BAD;
}

/* ------------------------------------------------------------------ */
/* Frames out                                                           */
/* ------------------------------------------------------------------ */

size_t hermes_ws_frame_header(uint8_t out[14], uint8_t opcode, size_t len, const uint8_t mask[4])
{
    size_t n = 0;
    out[n++] = (uint8_t)(0x80 | (opcode & 0x0F));
    if (len < 126) {
        out[n++] = (uint8_t)(0x80 | len);
    } else if (len <= 0xFFFF) {
        out[n++] = 0x80 | 126;
        out[n++] = (uint8_t)(len >> 8);
        out[n++] = (uint8_t)len;
    } else {
        out[n++] = 0x80 | 127;
        uint64_t big = (uint64_t)len;
        for (int shift = 56; shift >= 0; shift -= 8) out[n++] = (uint8_t)(big >> shift);
    }
    memcpy(out + n, mask, 4);
    return n + 4;
}

void hermes_ws_mask(uint8_t *data, size_t len, const uint8_t mask[4], size_t offset)
{
    for (size_t i = 0; i < len; i++) data[i] ^= mask[(offset + i) & 3];
}

/* ------------------------------------------------------------------ */
/* Frames in                                                            */
/* ------------------------------------------------------------------ */

void hermes_ws_decoder_init(hermes_ws_decoder_t *d, uint8_t *buf, size_t cap)
{
    memset(d, 0, sizeof(*d));
    d->msg = buf;
    d->msg_cap = cap;
    d->head_need = 2;
}

static hermes_ws_event_t fail(hermes_ws_decoder_t *d, uint16_t code, const char *why)
{
    d->failed = true;
    d->close_code = code;
    d->error = why;
    return HERMES_WS_ERROR;
}

static bool is_control(uint8_t opcode)
{
    return (opcode & 0x08) != 0;
}

/* The header is whole: check it, and get ready for its payload. */
static hermes_ws_event_t header_done(hermes_ws_decoder_t *d)
{
    const uint8_t *h = d->head;
    if (h[0] & 0x70) return fail(d, HERMES_WS_CLOSE_PROTOCOL, "a frame used an extension");
    d->fin = (h[0] & 0x80) != 0;
    d->opcode = h[0] & 0x0F;
    d->masked = (h[1] & 0x80) != 0;

    uint64_t len = h[1] & 0x7F;
    size_t at = 2;
    if (len == 126) {
        len = ((uint64_t)h[2] << 8) | h[3];
        at = 4;
    } else if (len == 127) {
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | h[2 + i];
        at = 10;
        if (len >> 63) return fail(d, HERMES_WS_CLOSE_PROTOCOL, "a frame's length is not a length");
    }
    if (d->masked) memcpy(d->mask, h + at, 4);
    d->left = len;
    d->done = 0;

    switch (d->opcode) {
    case HERMES_WS_OP_CLOSE:
    case HERMES_WS_OP_PING:
    case HERMES_WS_OP_PONG:
        if (!d->fin || len > sizeof(d->ctrl)) {
            return fail(d, HERMES_WS_CLOSE_PROTOCOL, "a control frame was split or too long");
        }
        d->ctrl_len = 0;
        break;
    case HERMES_WS_OP_TEXT:
    case HERMES_WS_OP_BINARY:
        if (d->message_opcode) {
            return fail(d, HERMES_WS_CLOSE_PROTOCOL, "a message began inside another");
        }
        d->message_opcode = d->opcode;
        d->msg_len = 0;
        d->message_ended = false;
        break;
    case HERMES_WS_OP_CONTINUATION:
        if (!d->message_opcode) {
            return fail(d, HERMES_WS_CLOSE_PROTOCOL, "a continuation continued nothing");
        }
        break;
    default:
        return fail(d, HERMES_WS_CLOSE_PROTOCOL, "a frame had an unknown opcode");
    }
    if (!is_control(d->opcode) && d->msg_len + len > d->msg_cap - 1) {
        return fail(d, HERMES_WS_CLOSE_TOO_BIG, "a message was too big for this device");
    }
    return HERMES_WS_NOTHING;
}

/* The payload is whole: say what arrived, if anything did. */
static hermes_ws_event_t payload_done(hermes_ws_decoder_t *d)
{
    d->head_len = 0;
    d->head_need = 2;
    switch (d->opcode) {
    case HERMES_WS_OP_PING:
        return HERMES_WS_PING;
    case HERMES_WS_OP_PONG:
        return HERMES_WS_PONG;
    case HERMES_WS_OP_CLOSE:
        d->close_code = d->ctrl_len >= 2 ? (uint16_t)((d->ctrl[0] << 8) | d->ctrl[1])
                                         : HERMES_WS_CLOSE_NO_STATUS;
        return HERMES_WS_CLOSE;
    default:
        if (!d->fin) return HERMES_WS_NOTHING;
        d->msg[d->msg_len] = '\0';
        hermes_ws_event_t event = d->message_opcode == HERMES_WS_OP_TEXT ? HERMES_WS_TEXT
                                                                         : HERMES_WS_BINARY;
        d->message_opcode = 0;
        d->message_ended = true;
        return event;
    }
}

size_t hermes_ws_decode(hermes_ws_decoder_t *d, const uint8_t *data, size_t len,
                        hermes_ws_event_t *event)
{
    *event = HERMES_WS_NOTHING;
    if (d->failed) {
        *event = HERMES_WS_ERROR;
        return len;
    }
    size_t used = 0;
    for (;;) {
        bool in_header = d->head_len < d->head_need;
        if (in_header) {
            if (used == len) return used;
            d->head[d->head_len++] = data[used++];
            if (d->head_len == 2) {
                uint8_t code = d->head[1] & 0x7F;
                d->head_need = (uint8_t)(2 + (code == 126 ? 2 : code == 127 ? 8 : 0) +
                                         ((d->head[1] & 0x80) ? 4 : 0));
            }
            if (d->head_len < d->head_need) continue;
            *event = header_done(d);
            if (*event == HERMES_WS_ERROR) return len;
            if (d->left == 0) {
                *event = payload_done(d);
                if (*event != HERMES_WS_NOTHING) return used;
            }
            continue;
        }

        if (used == len) return used;
        size_t take = len - used;
        if ((uint64_t)take > d->left) take = (size_t)d->left;
        uint8_t *to = is_control(d->opcode) ? d->ctrl + d->ctrl_len : d->msg + d->msg_len;
        memcpy(to, data + used, take);
        if (d->masked) hermes_ws_mask(to, take, d->mask, (size_t)d->done);
        if (is_control(d->opcode)) {
            d->ctrl_len += take;
        } else {
            d->msg_len += take;
        }
        used += take;
        d->done += take;
        d->left -= take;
        if (d->left == 0) {
            *event = payload_done(d);
            if (*event != HERMES_WS_NOTHING) return used;
        }
    }
}
