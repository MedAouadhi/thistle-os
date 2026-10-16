// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// The WebSocket client's framing: addresses, the opening handshake and its
// accept key (RFC 6455's own example), masked frames out, and frames in,
// whole, a byte at a time, fragmented, with a ping between the fragments,
// and the ways a frame can be wrong.

#include "hermes/hermes_ws.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            s_failures++;                                                \
        }                                                                \
    } while (0)

static void test_urls(void)
{
    hermes_ws_url_t u;
    char text[HERMES_WS_URL_MAX];

    EXPECT(hermes_ws_parse_url("ws://100.125.204.43:8765", &u) == NULL);
    EXPECT(strcmp(u.host, "100.125.204.43") == 0 && u.port == 8765 && strcmp(u.path, "/") == 0);
    hermes_ws_format_url(&u, text, sizeof text);
    EXPECT(strcmp(text, "ws://100.125.204.43:8765") == 0);

    /* What a person types: a peer's name, meaning the bridge's port. */
    EXPECT(hermes_ws_parse_url("  homelab \n", &u) == NULL);
    EXPECT(strcmp(u.host, "homelab") == 0 && u.port == HERMES_WS_DEFAULT_PORT);
    EXPECT(hermes_ws_parse_url("WS://homelab.tail1234.ts.net:9000/bridge", &u) == NULL);
    EXPECT(strcmp(u.host, "homelab.tail1234.ts.net") == 0 && u.port == 9000);
    EXPECT(strcmp(u.path, "/bridge") == 0);
    hermes_ws_format_url(&u, text, sizeof text);
    EXPECT(strcmp(text, "ws://homelab.tail1234.ts.net:9000/bridge") == 0);

    EXPECT(hermes_ws_parse_url("wss://homelab", &u) != NULL);
    EXPECT(hermes_ws_parse_url("http://homelab", &u) != NULL);
    EXPECT(hermes_ws_parse_url("ws://", &u) != NULL);
    EXPECT(hermes_ws_parse_url("", &u) != NULL);
    EXPECT(hermes_ws_parse_url(NULL, &u) != NULL);
    EXPECT(hermes_ws_parse_url("homelab:0", &u) != NULL);
    EXPECT(hermes_ws_parse_url("homelab:65536", &u) != NULL);
    EXPECT(hermes_ws_parse_url("homelab:", &u) != NULL);
    EXPECT(hermes_ws_parse_url("home lab", &u) != NULL);
    EXPECT(hermes_ws_parse_url("homelab:80x", &u) != NULL);
}

static void test_handshake(void)
{
    /* RFC 6455 section 1.3. */
    char key[25], accept[29];
    hermes_ws_make_key((const uint8_t *)"the sample nonce", key);
    EXPECT(strcmp(key, "dGhlIHNhbXBsZSBub25jZQ==") == 0);
    hermes_ws_accept(key, accept);
    EXPECT(strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);

    hermes_ws_url_t u;
    hermes_ws_parse_url("ws://100.64.0.1:8765", &u);
    char request[512];
    int n = hermes_ws_request(request, sizeof request, &u, key);
    EXPECT(n > 0 && (size_t)n == strlen(request));
    EXPECT(strncmp(request, "GET / HTTP/1.1\r\nHost: 100.64.0.1:8765\r\n", 39) == 0);
    EXPECT(strstr(request, "\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") != NULL);
    EXPECT(strstr(request, "\r\nSec-WebSocket-Version: 13\r\n\r\n") != NULL);
    EXPECT(hermes_ws_request(request, 40, &u, key) == -1);

    int status;
    size_t head;
    const char *partial = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n";
    EXPECT(hermes_ws_response(partial, strlen(partial), key, &status, &head) ==
           HERMES_WS_HEAD_INCOMPLETE);

    /* As Python's websockets answers, and a frame already behind it. */
    const char *ok = "HTTP/1.1 101 Switching Protocols\r\n"
                     "date: Thu, 16 Oct 2026 10:00:00 GMT\r\n"
                     "upgrade: WebSocket\r\n"
                     "connection: keep-alive, Upgrade\r\n"
                     "sec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                     "server: Python/3.12 websockets/15.0.1\r\n\r\n\x81\x00";
    size_t ok_len = strlen(ok) + 1;
    EXPECT(hermes_ws_response(ok, ok_len, key, &status, &head) == HERMES_WS_HEAD_OK);
    EXPECT(status == 101 && head == ok_len - 2);

    const char *wrong = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                        "Connection: Upgrade\r\nSec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n";
    EXPECT(hermes_ws_response(wrong, strlen(wrong), key, &status, &head) == HERMES_WS_HEAD_BAD);

    const char *forbidden = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
    EXPECT(hermes_ws_response(forbidden, strlen(forbidden), key, &status, &head) ==
           HERMES_WS_HEAD_REFUSED);
    EXPECT(status == 403);

    const char *junk = "SSH-2.0-OpenSSH_9.6\r\n\r\n";
    EXPECT(hermes_ws_response(junk, strlen(junk), key, &status, &head) == HERMES_WS_HEAD_BAD);
}

static void test_frames_out(void)
{
    /* RFC 6455 section 5.7: "Hello", masked with 37 fa 21 3d. */
    const uint8_t mask[4] = { 0x37, 0xfa, 0x21, 0x3d };
    uint8_t head[14];
    EXPECT(hermes_ws_frame_header(head, HERMES_WS_OP_TEXT, 5, mask) == 6);
    EXPECT(head[0] == 0x81 && head[1] == 0x85 && memcmp(head + 2, mask, 4) == 0);
    uint8_t hello[] = "Hello";
    hermes_ws_mask(hello, 5, mask, 0);
    EXPECT(memcmp(hello, "\x7f\x9f\x4d\x51\x58", 5) == 0);

    /* Masked in two pieces, as the sender does a chunk at a time. */
    uint8_t again[] = "Hello";
    hermes_ws_mask(again, 2, mask, 0);
    hermes_ws_mask(again + 2, 3, mask, 2);
    EXPECT(memcmp(again, "\x7f\x9f\x4d\x51\x58", 5) == 0);

    EXPECT(hermes_ws_frame_header(head, HERMES_WS_OP_TEXT, 126, mask) == 8);
    EXPECT(head[1] == (0x80 | 126) && head[2] == 0 && head[3] == 126);
    EXPECT(hermes_ws_frame_header(head, HERMES_WS_OP_BINARY, 70000, mask) == 14);
    EXPECT(head[0] == 0x82 && head[1] == (0x80 | 127));
    EXPECT(head[7] == 0x01 && head[8] == 0x11 && head[9] == 0x70);
}

typedef struct {
    hermes_ws_event_t events[8];
    char texts[8][64];
    size_t count;
} seen_t;

/* Feeds `len` bytes `step` at a time, noting every event. */
static void feed(hermes_ws_decoder_t *d, const uint8_t *data, size_t len, size_t step, seen_t *seen)
{
    size_t at = 0;
    while (at < len) {
        size_t n = len - at < step ? len - at : step;
        size_t used = 0;
        while (used < n) {
            hermes_ws_event_t event;
            size_t took = hermes_ws_decode(d, data + at + used, n - used, &event);
            used += took;
            if (event != HERMES_WS_NOTHING && seen->count < 8) {
                seen->events[seen->count] = event;
                if (event == HERMES_WS_TEXT) {
                    snprintf(seen->texts[seen->count], 64, "%s", (const char *)d->msg);
                } else if (event == HERMES_WS_PING) {
                    snprintf(seen->texts[seen->count], 64, "%.*s", (int)d->ctrl_len,
                             (const char *)d->ctrl);
                }
                seen->count++;
            }
            if (event == HERMES_WS_ERROR) return;
            if (took == 0) break;
        }
        at += n;
    }
}

static void test_frames_in(void)
{
    uint8_t buf[64];
    hermes_ws_decoder_t d;

    /* Two messages in one read, then the same a byte at a time. */
    const uint8_t two[] = { 0x81, 0x05, 'H', 'e', 'l', 'l', 'o', 0x81, 0x02, 'h', 'i' };
    for (size_t step = 1; step <= sizeof two; step += sizeof two - 1) {
        seen_t seen = { 0 };
        hermes_ws_decoder_init(&d, buf, sizeof buf);
        feed(&d, two, sizeof two, step, &seen);
        EXPECT(seen.count == 2);
        EXPECT(seen.events[0] == HERMES_WS_TEXT && strcmp(seen.texts[0], "Hello") == 0);
        EXPECT(seen.events[1] == HERMES_WS_TEXT && strcmp(seen.texts[1], "hi") == 0);
    }

    /* Fragmented, with a ping between the fragments (section 5.4), and a
     * frame the server masked, which is wrong of it but readable. */
    const uint8_t fragmented[] = {
        0x01, 0x03, 'H', 'e', 'l',
        0x89, 0x02, 'p', '!',
        0x80, 0x82, 0x37, 0xfa, 0x21, 0x3d, 'l' ^ 0x37, 'o' ^ 0xfa,
    };
    for (size_t step = 1; step <= 2; step++) {
        seen_t seen = { 0 };
        hermes_ws_decoder_init(&d, buf, sizeof buf);
        feed(&d, fragmented, sizeof fragmented, step, &seen);
        EXPECT(seen.count == 2);
        EXPECT(seen.events[0] == HERMES_WS_PING && strcmp(seen.texts[0], "p!") == 0);
        EXPECT(seen.events[1] == HERMES_WS_TEXT && strcmp(seen.texts[1], "Hello") == 0);
    }

    /* A 16-bit length. */
    uint8_t medium[4 + 130];
    medium[0] = 0x82;
    medium[1] = 126;
    medium[2] = 0;
    medium[3] = 130;
    memset(medium + 4, 'x', 130);
    uint8_t big[200];
    seen_t seen = { 0 };
    hermes_ws_decoder_init(&d, big, sizeof big);
    feed(&d, medium, sizeof medium, 7, &seen);
    EXPECT(seen.count == 1 && seen.events[0] == HERMES_WS_BINARY && d.msg_len == 130);

    /* An empty message, and a close with its code. */
    const uint8_t closing[] = { 0x81, 0x00, 0x88, 0x02, 0x03, 0xE8 };
    memset(&seen, 0, sizeof seen);
    hermes_ws_decoder_init(&d, buf, sizeof buf);
    feed(&d, closing, sizeof closing, sizeof closing, &seen);
    EXPECT(seen.count == 2 && seen.events[0] == HERMES_WS_TEXT && seen.texts[0][0] == '\0');
    EXPECT(seen.events[1] == HERMES_WS_CLOSE && d.close_code == 1000);
    const uint8_t bare_close[] = { 0x88, 0x00 };
    memset(&seen, 0, sizeof seen);
    hermes_ws_decoder_init(&d, buf, sizeof buf);
    feed(&d, bare_close, sizeof bare_close, 1, &seen);
    EXPECT(seen.count == 1 && d.close_code == HERMES_WS_CLOSE_NO_STATUS);
}

static void test_frames_wrong(void)
{
    uint8_t buf[16];
    hermes_ws_decoder_t d;
    hermes_ws_event_t event;

    /* A message bigger than the buffer, known from its header. */
    const uint8_t too_big[] = { 0x81, 0x7E, 0x00, 0x20 };
    hermes_ws_decoder_init(&d, buf, sizeof buf);
    hermes_ws_decode(&d, too_big, sizeof too_big, &event);
    EXPECT(event == HERMES_WS_ERROR && d.close_code == HERMES_WS_CLOSE_TOO_BIG);
    /* And nothing more is read after it. */
    hermes_ws_decode(&d, (const uint8_t *)"\x81\x00", 2, &event);
    EXPECT(event == HERMES_WS_ERROR);

    /* Fragments adding up to more than fits. */
    const uint8_t grows[] = { 0x01, 0x0A, '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 0x80, 0x0A };
    hermes_ws_decoder_init(&d, buf, sizeof buf);
    size_t used = hermes_ws_decode(&d, grows, 12, &event);
    EXPECT(event == HERMES_WS_NOTHING && used == 12);
    hermes_ws_decode(&d, grows + used, sizeof grows - used, &event);
    EXPECT(event == HERMES_WS_ERROR && d.close_code == HERMES_WS_CLOSE_TOO_BIG);

    const uint8_t *wrong[] = {
        (const uint8_t *)"\x80\x01x",      /* a continuation of nothing */
        (const uint8_t *)"\xC1\x01x",      /* RSV1: an extension nobody agreed to */
        (const uint8_t *)"\x83\x01x",      /* an unknown opcode */
        (const uint8_t *)"\x09\x01x",      /* a ping split into fragments */
        (const uint8_t *)"\x89\x7E\x00\x7E", /* a ping too long for a control frame */
    };
    for (size_t i = 0; i < sizeof wrong / sizeof wrong[0]; i++) {
        hermes_ws_decoder_init(&d, buf, sizeof buf);
        hermes_ws_decode(&d, wrong[i], 4, &event);
        EXPECT(event == HERMES_WS_ERROR && d.close_code == HERMES_WS_CLOSE_PROTOCOL);
    }

    /* A new message before the last one ended. */
    const uint8_t twice[] = { 0x01, 0x01, 'a', 0x81, 0x01, 'b' };
    hermes_ws_decoder_init(&d, buf, sizeof buf);
    used = hermes_ws_decode(&d, twice, 3, &event);
    EXPECT(event == HERMES_WS_NOTHING && used == 3);
    hermes_ws_decode(&d, twice + used, sizeof twice - used, &event);
    EXPECT(event == HERMES_WS_ERROR && d.close_code == HERMES_WS_CLOSE_PROTOCOL);
}

int main(void)
{
    test_urls();
    test_handshake();
    test_frames_out();
    test_frames_in();
    test_frames_wrong();
    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_hermes_ws: all checks passed\n");
    return 0;
}
