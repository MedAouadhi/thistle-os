/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Tailnet chat's hardware spike (ticket 01), a debug switch
 *
 * Off unless CONFIG_THISTLE_TAILNET_CHAT_SPIKE is set (components/kernel/
 * Kconfig, "ThistleOS debug"), and then main.c calls tailnet_chat_spike_start()
 * once the device is up. It is throwaway: the Tailnet chat module replaces
 * it. While the tailnet is connected it
 *
 * - listens on TCP port 4810 and answers a WebSocket on any path, echoing
 *   each text or binary frame back, admitting only a connection from the
 *   tailnet's 100.64.0.0/10 that arrived on the device's own tailnet
 *   address, and closing anything else before the handshake;
 * - logs the lwIP sockets open, and the internal RAM free, as connections
 *   come and go;
 * - once, on a task of its own, makes an Opus encoder and decoder and times
 *   20 ms frames of speech through them, logging the CPU time per frame, the
 *   task's stack and the pseudostack used, and the internal RAM each step
 *   took.
 *
 * It switches WiFi and the tailnet on, if they are off, and saves that.
 *
 * Everything runs as a job on the kernel worker, its sockets non-blocking,
 * as the module will. The log's tag is "chat_spike".
 */
#include "sdkconfig.h"

#include <stdbool.h>

void tailnet_chat_spike_start(void);

#if !CONFIG_THISTLE_TAILNET_CHAT_SPIKE

void tailnet_chat_spike_start(void) {}

#else

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"

#include "thistle/kworker.h"
#include "thistle/power_manager.h"
#include "thistle/tailnet.h"
#include "thistle/wifi_manager.h"
#include "thistle_opus.h"

static const char *TAG = "chat_spike";

#define PORT 4810
#define POLL_MS 10
/* More than the module's 4, so that the count goes past them */
#define MAX_CONNS 6
#define BUF_SIZE 4096
#define BENCH_STACK (24 * 1024)
#define BENCH_FRAMES 100

typedef enum { FREE, HANDSHAKE, OPEN } conn_state_t;

typedef struct {
    conn_state_t state;
    int fd;
    char peer[16];
    int64_t accepted_us;
    size_t have;
    uint8_t *buf; /* BUF_SIZE, PSRAM */
    uint32_t frames;
} conn_t;

static conn_t s_conns[MAX_CONNS];
static int s_listen = -1;
static char s_ip[16];
static bool s_bench_started;
static uint32_t s_hold;

static size_t internal_free(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/* The lwIP sockets open now, of CONFIG_LWIP_MAX_SOCKETS */
static int sockets_open(void)
{
    int n = 0;
    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++) {
        if (lwip_fcntl(fd, F_GETFL, 0) >= 0) {
            n++;
        }
    }
    return n;
}

static int conns_open(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CONNS; i++) {
        n += s_conns[i].state != FREE;
    }
    return n;
}

static void log_counts(const char *why)
{
    ESP_LOGI(TAG, "%s: %d chat connections, %d of %d lwIP sockets open; internal RAM %u KB free",
             why, conns_open(), sockets_open(), CONFIG_LWIP_MAX_SOCKETS,
             (unsigned)(internal_free() / 1024));
}

static void conn_close(conn_t *c, const char *why)
{
    if (c->state == FREE) {
        return;
    }
    close(c->fd);
    ESP_LOGI(TAG, "%s closed (%s) after %u frames", c->peer, why, (unsigned)c->frames);
    c->state = FREE;
    c->fd = -1;
    c->have = 0;
    log_counts("closed");
}

static bool ip_of(const struct sockaddr_storage *ss, char *out, size_t len, uint32_t *v4)
{
    const uint8_t *a = NULL;
    if (ss->ss_family == AF_INET) {
        a = (const uint8_t *)&((const struct sockaddr_in *)ss)->sin_addr;
    }
    if (!a) {
        return false;
    }
    snprintf(out, len, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    *v4 = ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3];
    return true;
}

static void listen_stop(void)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        conn_close(&s_conns[i], "the tailnet went down");
    }
    if (s_listen >= 0) {
        close(s_listen);
        s_listen = -1;
        ESP_LOGI(TAG, "stopped listening");
    }
    s_ip[0] = '\0';
}

static void listen_start(const char *ip)
{
    size_t before = internal_free();
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGE(TAG, "no socket: %d", errno);
        return;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    /* Any address, so that a connection to the WiFi one arrives and can be
     * seen refused; the module will do the same check. */
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(PORT)};
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 4) != 0) {
        ESP_LOGE(TAG, "cannot listen on %d: %d", PORT, errno);
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    s_listen = fd;
    strlcpy(s_ip, ip, sizeof s_ip);
    ESP_LOGI(TAG, "listening on %s:%d; internal RAM %u KB free before, %u KB after", s_ip, PORT,
             (unsigned)(before / 1024), (unsigned)(internal_free() / 1024));
    log_counts("listening");
}

static void accept_one(void)
{
    struct sockaddr_storage peer;
    socklen_t plen = sizeof peer;
    int fd = accept(s_listen, (struct sockaddr *)&peer, &plen);
    if (fd < 0) {
        return;
    }
    struct sockaddr_storage local;
    socklen_t llen = sizeof local;
    char from[16] = "?", to[16] = "?";
    uint32_t from_v4 = 0, to_v4 = 0;
    ip_of(&peer, from, sizeof from, &from_v4);
    if (getsockname(fd, (struct sockaddr *)&local, &llen) == 0) {
        ip_of(&local, to, sizeof to, &to_v4);
    }
    bool tailnet_peer = (from_v4 & 0xFFC00000u) == 0x64400000u; /* 100.64.0.0/10 */
    bool on_tailnet = strcmp(to, s_ip) == 0;
    if (!tailnet_peer || !on_tailnet) {
        ESP_LOGW(TAG, "refused %s, to %s: %s", from, to,
                 !tailnet_peer ? "not from the tailnet" : "not on the tailnet's address");
        close(fd);
        return;
    }
    conn_t *c = NULL;
    for (int i = 0; i < MAX_CONNS && !c; i++) {
        if (s_conns[i].state == FREE) {
            c = &s_conns[i];
        }
    }
    if (!c) {
        ESP_LOGW(TAG, "refused %s: %d connections open", from, MAX_CONNS);
        close(fd);
        return;
    }
    if (!c->buf) {
        c->buf = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!c->buf) {
            close(fd);
            return;
        }
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
    c->fd = fd;
    c->state = HANDSHAKE;
    c->have = 0;
    c->frames = 0;
    c->accepted_us = esp_timer_get_time();
    strlcpy(c->peer, from, sizeof c->peer);
    ESP_LOGI(TAG, "accepted %s on %s", from, to);
    log_counts("accepted");
}

static bool send_all(int fd, const uint8_t *data, size_t len)
{
    int64_t give_up = esp_timer_get_time() + 2000000;
    while (len) {
        int n = send(fd, data, len, 0);
        if (n > 0) {
            data += n;
            len -= (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
                   esp_timer_get_time() < give_up) {
            vTaskDelay(1);
        } else {
            return false;
        }
    }
    return true;
}

/* The request is whole: answer it with the accept key, RFC 6455 4.2.2. */
static void handshake(conn_t *c)
{
    c->buf[c->have] = '\0';
    char *end = strstr((char *)c->buf, "\r\n\r\n");
    if (!end) {
        if (c->have >= BUF_SIZE - 1) {
            conn_close(c, "request too long");
        }
        return;
    }
    const char *k = strcasestr((char *)c->buf, "\r\nSec-WebSocket-Key:");
    if (!k) {
        conn_close(c, "not a WebSocket");
        return;
    }
    k += strlen("\r\nSec-WebSocket-Key:");
    while (*k == ' ') {
        k++;
    }
    const char *ke = strstr(k, "\r\n");
    char key[96];
    size_t kl = (size_t)(ke - k);
    if (kl == 0 || kl > 64) {
        conn_close(c, "a bad key");
        return;
    }
    memcpy(key, k, kl);
    strcpy(key + kl, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    uint8_t sha[20];
    mbedtls_sha1((const uint8_t *)key, strlen(key), sha);
    char accept[32];
    size_t al = 0;
    mbedtls_base64_encode((uint8_t *)accept, sizeof accept, &al, sha, sizeof sha);
    accept[al] = '\0';
    char resp[160];
    int rl = snprintf(resp, sizeof resp,
                      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
                      accept);
    size_t rest = c->have - (size_t)((uint8_t *)end + 4 - c->buf);
    memmove(c->buf, end + 4, rest);
    c->have = rest;
    if (!send_all(c->fd, (const uint8_t *)resp, (size_t)rl)) {
        conn_close(c, "the answer would not go");
        return;
    }
    c->state = OPEN;
    ESP_LOGI(TAG, "%s: WebSocket open, %lld ms after the accept", c->peer,
             (long long)((esp_timer_get_time() - c->accepted_us) / 1000));
}

/* Echo each whole frame in the buffer: unmasked, as a server sends. */
static void frames(conn_t *c)
{
    for (;;) {
        if (c->have < 2) {
            return;
        }
        uint8_t *b = c->buf;
        uint8_t op = b[0] & 0x0F;
        bool masked = b[1] & 0x80;
        size_t len = b[1] & 0x7F, hdr = 2;
        if (len == 126) {
            if (c->have < 4) {
                return;
            }
            len = ((size_t)b[2] << 8) | b[3];
            hdr = 4;
        } else if (len == 127) {
            conn_close(c, "a frame too big");
            return;
        }
        size_t need = hdr + (masked ? 4 : 0) + len;
        if (need > BUF_SIZE) {
            conn_close(c, "a frame too big");
            return;
        }
        if (c->have < need) {
            return;
        }
        uint8_t *payload = b + hdr + (masked ? 4 : 0);
        if (masked) {
            const uint8_t *m = b + hdr;
            for (size_t i = 0; i < len; i++) {
                payload[i] ^= m[i & 3];
            }
        }
        c->frames++;
        if (op == 0x8) {
            uint8_t closing[2] = {0x88, 0};
            send_all(c->fd, closing, sizeof closing);
            conn_close(c, "the peer closed");
            return;
        }
        uint8_t out_op = op == 0x9 ? 0xA : op; /* a ping is answered by a pong */
        if (op != 0xA) {
            uint8_t head[4] = {(uint8_t)(0x80 | out_op), 0};
            size_t hl = 2;
            if (len < 126) {
                head[1] = (uint8_t)len;
            } else {
                head[1] = 126;
                head[2] = (uint8_t)(len >> 8);
                head[3] = (uint8_t)len;
                hl = 4;
            }
            if (!send_all(c->fd, head, hl) || !send_all(c->fd, payload, len)) {
                conn_close(c, "the echo would not go");
                return;
            }
        }
        memmove(b, b + need, c->have - need);
        c->have -= need;
    }
}

static void conn_poll(conn_t *c)
{
    for (;;) {
        if (c->have >= BUF_SIZE - 1) {
            conn_close(c, "the buffer is full");
            return;
        }
        int n = recv(c->fd, c->buf + c->have, BUF_SIZE - 1 - c->have, 0);
        if (n == 0) {
            conn_close(c, "the peer went");
            return;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                conn_close(c, "an error");
            }
            return;
        }
        c->have += (size_t)n;
        if (c->state == HANDSHAKE) {
            handshake(c);
        }
        if (c->state == OPEN) {
            frames(c);
        }
        if (c->state == FREE) {
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Opus on the S3                                                       */
/* ------------------------------------------------------------------ */

static void bench_clip(int16_t *pcm, int n)
{
    float phase = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / THISTLE_OPUS_RATE;
        float f0 = 120.0f + 60.0f * (0.5f + 0.5f * sinf(2 * (float)M_PI * 0.5f * t));
        phase += 2 * (float)M_PI * f0 / THISTLE_OPUS_RATE;
        float v = 0;
        for (int h = 1; h * f0 < 3800; h++) {
            float f = h * f0;
            float d1 = (f - 700) / 150, d2 = (f - 1200) / 200;
            v += (1.0f / (1 + d1 * d1) + 0.6f / (1 + d2 * d2) + 0.05f) * sinf(h * phase) / h;
        }
        float s = sinf(2 * (float)M_PI * 4.0f * t);
        pcm[i] = (int16_t)(9000.0f * (s > 0 ? s : 0) * v);
    }
}

static void bench_one(const int16_t *clip, int complexity, bool scratch_internal)
{
    thistle_opus_place(false);
    size_t ram0 = internal_free();
    OpusEncoder *enc = thistle_opus_encoder_new(THISTLE_OPUS_BITRATE);
    OpusDecoder *dec = thistle_opus_decoder_new();
    size_t ram1 = internal_free();
    if (!enc || !dec) {
        ESP_LOGE(TAG, "opus: no encoder or decoder");
        thistle_opus_encoder_free(enc);
        thistle_opus_decoder_free(dec);
        return;
    }
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity));
    thistle_opus_place(scratch_internal);
    int16_t out[THISTLE_OPUS_FRAME];
    uint8_t packet[THISTLE_OPUS_MAX_PACKET];
    int64_t enc_sum = 0, enc_max = 0, dec_sum = 0, dec_max = 0;
    size_t bytes = 0;
    for (int f = 0; f < BENCH_FRAMES; f++) {
        int64_t t0 = esp_timer_get_time();
        int len = opus_encode(enc, clip + f * THISTLE_OPUS_FRAME, THISTLE_OPUS_FRAME, packet,
                              sizeof packet);
        int64_t t1 = esp_timer_get_time();
        int got = len > 0 ? opus_decode(dec, packet, len, out, THISTLE_OPUS_FRAME, 0) : -1;
        int64_t t2 = esp_timer_get_time();
        if (len <= 0 || got != THISTLE_OPUS_FRAME) {
            ESP_LOGE(TAG, "opus: frame %d failed (%d, %d)", f, len, got);
            break;
        }
        bytes += (size_t)len;
        enc_sum += t1 - t0;
        dec_sum += t2 - t1;
        enc_max = t1 - t0 > enc_max ? t1 - t0 : enc_max;
        dec_max = t2 - t1 > dec_max ? t2 - t1 : dec_max;
    }
    /* A lost frame, concealed */
    int64_t t3 = esp_timer_get_time();
    opus_decode(dec, NULL, 0, out, THISTLE_OPUS_FRAME, 0);
    int64_t plc = esp_timer_get_time() - t3;
    ESP_LOGI(TAG,
             "opus complexity %d: encode %lld us a 20 ms frame (max %lld), decode %lld us (max "
             "%lld), conceal %lld us; %u bytes for 2 s",
             complexity, (long long)(enc_sum / BENCH_FRAMES), (long long)enc_max,
             (long long)(dec_sum / BENCH_FRAMES), (long long)dec_max, (long long)plc,
             (unsigned)bytes);
    ESP_LOGI(TAG,
             "opus: encoder %d B, decoder %d B (PSRAM); internal RAM %u KB free before them, %u KB "
             "after; pseudostack %u of %u B used",
             opus_encoder_get_size(1), opus_decoder_get_size(1), (unsigned)(ram0 / 1024),
             (unsigned)(ram1 / 1024), (unsigned)thistle_opus_scratch_used(),
             (unsigned)thistle_opus_scratch_size());
    thistle_opus_encoder_free(enc);
    thistle_opus_decoder_free(dec);
}

static void bench_task(void *arg)
{
    (void)arg;
    size_t n = BENCH_FRAMES * THISTLE_OPUS_FRAME;
    int16_t *clip = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (clip) {
        bench_clip(clip, (int)n);
        UBaseType_t before = uxTaskGetStackHighWaterMark(NULL);
        static const int complexities[] = {9, 5, 3, 1, 0};
        for (int internal = 0; internal < 2; internal++) {
            ESP_LOGI(TAG, "opus: states in PSRAM, pseudostack in %s; internal RAM largest block %u KB", internal ? "internal RAM" : "PSRAM", (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
            for (size_t i = 0; i < sizeof complexities / sizeof complexities[0]; i++) {
                bench_one(clip, complexities[i], internal);
            }
        }
        thistle_opus_place(false);
        ESP_LOGI(TAG, "opus task: %u B stack, %u B never used (%u before the codecs)",
                 (unsigned)BENCH_STACK, (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 (unsigned)before);
        free(clip);
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */

/* The peer list as the kernel exposes it, with each peer's online state */
static void log_peers(void)
{
    int n = tailnet_peer_count();
    int online = 0;
    for (int i = 0; i < n; i++) {
        tailnet_peer_t p;
        if (tailnet_get_peer(i, &p) == ESP_OK) {
            online += p.online;
            ESP_LOGI(TAG, "peer %s %s: %s%s", p.ip, p.name, p.online ? "online" : "offline",
                     p.direct ? ", direct" : "");
        }
    }
    ESP_LOGI(TAG, "%d peers, %d online", n, online);
}

static void spike_job(void *arg)
{
    (void)arg;
    tailnet_status_t st;
    bool up = tailnet_is_connected() && tailnet_get_status(&st) == ESP_OK && st.ip[0];
    if (!up) {
        if (s_listen >= 0) {
            listen_stop();
        }
        return;
    }
    if (s_listen >= 0 && strcmp(st.ip, s_ip) != 0) {
        listen_stop();
    }
    if (s_listen < 0) {
        log_counts("the tailnet is up");
        listen_start(st.ip);
        if (s_listen < 0) {
            return;
        }
        if (!s_bench_started) {
            s_bench_started = true;
            size_t before = internal_free();
            if (xTaskCreate(bench_task, "opus_bench", BENCH_STACK, NULL, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "no opus task");
            } else {
                ESP_LOGI(TAG, "opus task made: internal RAM %u KB free before, %u KB after",
                         (unsigned)(before / 1024), (unsigned)(internal_free() / 1024));
            }
        }
    }
    static int64_t peers_logged_us;
    if (peers_logged_us == 0 || esp_timer_get_time() - peers_logged_us > 60 * 1000000LL) {
        peers_logged_us = esp_timer_get_time();
        log_peers();
    }
    accept_one();
    for (int i = 0; i < MAX_CONNS; i++) {
        if (s_conns[i].state != FREE) {
            conn_poll(&s_conns[i]);
        }
    }
}

void tailnet_chat_spike_start(void)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        s_conns[i].fd = -1;
    }
    s_hold = power_manager_hold("chat spike");
    /* The spike needs a network and the tailnet: a board with them switched
     * off has them switched on, and saved so, as Settings would. */
    if (!wifi_manager_is_enabled()) {
        ESP_LOGW(TAG, "WiFi was switched off; switching it on (%s)",
                 esp_err_to_name(wifi_manager_set_enabled(true)));
    }
    if (tailnet_available() && !tailnet_is_enabled()) {
        ESP_LOGW(TAG, "the tailnet was switched off; switching it on (%s)",
                 esp_err_to_name(tailnet_set_enabled(true)));
    }
    ESP_LOGW(TAG, "Tailnet chat spike on (CONFIG_THISTLE_TAILNET_CHAT_SPIKE): echo on port %d", PORT);
    kworker_every(spike_job, NULL, POLL_MS);
}

#endif /* CONFIG_THISTLE_TAILNET_CHAT_SPIKE */
