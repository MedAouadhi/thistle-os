/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Hermes app's Bridge Session. See hermes_bridge.h.
 *
 * A session is one allocation in PSRAM: the chat state, the receive buffer,
 * the outbox and the connection. Its task owns the socket; the screens
 * reach the rest through s_lock and s_current. Stopping detaches the
 * session from s_current under the lock, so nothing else can reach it
 * after, and the task frees it as it ends. A task held up in a name lookup
 * therefore costs a stop nothing: it finds itself stopped and goes.
 */
#include "hermes/hermes_bridge.h"

#include "thistle/power_manager.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#ifdef SIMULATOR_BUILD
#include "esp_system.h"
#else
#include "esp_random.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hermes";

#define APP_VERSION        "0.1.0"
/* The bridge's own limit on a frame. */
#define RX_BYTES           (64 * 1024)
#define OUTBOX_SLOTS       6
/* A draft of HERMES_DRAFT_MAX bytes, escaped, and the frame around it. */
#define OUTBOX_BYTES       4608
#define TASK_STACK         8192
#define TASK_PRIORITY      5
#define CONNECT_MS         10000
#define HELLO_MS           15000
/* The bridge pings every 20 s; nothing at all for this long is a lost link. */
#define SILENCE_MS         50000
#define SEND_MS            5000
#define POLL_MS            100
#define BACKOFF_MAX_S      30
#define REFUSED_RETRY_S    30
/* A turn holds the device awake for this long at most. */
#define HOLD_MAX_US        (10LL * 60 * 1000 * 1000)
/* What a draft may hold, which the bridge allows 8 KB of. */
#define HERMES_DRAFT_MAX   2000

typedef struct {
    bool used;
    bool sending;
    uint32_t sequence;
    size_t len;
    char text[OUTBOX_BYTES];
} slot_t;

typedef struct {
    volatile bool stop;
    volatile bool retry_now;
    hermes_ws_url_t url;
    hermes_link_status_t status;
    hermes_chat_t chat;
    hermes_ws_conn_t conn;
    slot_t outbox[OUTBOX_SLOTS];
    uint32_t sequence;
    char stop_request[HERMES_CHAT_REQUEST_BYTES];
    uint32_t hold;
    int64_t hold_since_us;
    uint8_t rx[RX_BYTES];
} session_t;

static SemaphoreHandle_t s_lock;
static session_t *s_current;
static uint32_t s_generation;

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* Appends `part` to out[0..*n), cut between characters to fit `cap`. */
static void append_cut(char *out, size_t cap, size_t *n, const char *part)
{
    size_t len = strlen(part);
    if (*n + len >= cap) {
        len = cap - 1 - *n;
        while (len > 0 && ((unsigned char)part[len] & 0xC0) == 0x80) len--;
    }
    memcpy(out + *n, part, len);
    *n += len;
    out[*n] = '\0';
}

/* `prefix` then `text` into `out`, cut between characters to fit. */
static void put_reason(char *out, size_t cap, const char *prefix, const char *text)
{
    size_t n = 0;
    out[0] = '\0';
    append_cut(out, cap, &n, prefix);
    append_cut(out, cap, &n, text);
}

static void new_request_id(char out[HERMES_CHAT_REQUEST_BYTES])
{
    snprintf(out, HERMES_CHAT_REQUEST_BYTES, "%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random());
}

/* ------------------------------------------------------------------ */
/* The session's own side: only its task calls these                    */
/* ------------------------------------------------------------------ */

static void set_link(session_t *s, hermes_link_t link, const char *reason)
{
    lock();
    s->status.link = link;
    if (reason) put_reason(s->status.reason, sizeof(s->status.reason), "", reason);
    s->status.retry_in_s = 0;
    hermes_chat_touch(&s->chat, NULL);
    unlock();
}

/* Waits `seconds`, or until stopped or told to try now. */
static void wait_to_retry(session_t *s, uint32_t seconds)
{
    s->retry_now = false;
    int64_t until = now_ms() + (int64_t)seconds * 1000;
    while (!s->stop && !s->retry_now) {
        int64_t left = until - now_ms();
        if (left <= 0) break;
        lock();
        /* The status is read apart from the chat state, so this costs the
         * screens no copy of it. */
        s->status.retry_in_s = (uint32_t)((left + 999) / 1000);
        unlock();
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

/* Holds the device awake while a turn is in flight, ten minutes at most. */
static void keep_awake(session_t *s, bool busy)
{
    if (busy && !s->hold && s->hold_since_us == 0) {
        s->hold = power_manager_hold("Hermes is answering");
        s->hold_since_us = esp_timer_get_time();
    } else if (s->hold && (!busy || esp_timer_get_time() - s->hold_since_us > HOLD_MAX_US)) {
        power_manager_release(s->hold);
        s->hold = 0;
    }
    if (!busy) s->hold_since_us = 0;
}

/* Sends the oldest queued frame, if there is one. False if the link broke. */
static bool send_next(session_t *s)
{
    lock();
    slot_t *next = NULL;
    for (int i = 0; i < OUTBOX_SLOTS; i++) {
        slot_t *slot = &s->outbox[i];
        if (slot->used && !slot->sending && (!next || slot->sequence < next->sequence)) next = slot;
    }
    if (next) next->sending = true;
    unlock();
    if (!next) return true;

    bool sent = hermes_ws_conn_send_text(&s->conn, next->text, next->len, SEND_MS);
    char type[32] = "";
    const char *t = strstr(next->text, "\"type\":\"");
    if (t) sscanf(t + 8, "%31[A-Z_]", type);
    if (sent) ESP_LOGI(TAG, "sent %s", type);
    else ESP_LOGW(TAG, "could not send %s: %s", type, s->conn.error);

    lock();
    next->used = next->sending = false;
    unlock();
    return sent;
}

static void forget_outbox(session_t *s)
{
    lock();
    for (int i = 0; i < OUTBOX_SLOTS; i++) s->outbox[i].used = s->outbox[i].sending = false;
    unlock();
}

static const char *open_reason(hermes_ws_open_t open, const hermes_ws_conn_t *c,
                               const hermes_ws_url_t *url, char *out, size_t cap)
{
    switch (open) {
    case HERMES_WS_NO_HOST:
        snprintf(out, cap, "%s is not a name on the tailnet.", url->host);
        break;
    case HERMES_WS_REFUSED:
        snprintf(out, cap, "%s answered, but no bridge listens on port %u. Is Hermes running?",
                 url->host, (unsigned)url->port);
        break;
    case HERMES_WS_TIMED_OUT:
        snprintf(out, cap, "Nothing answered at %s:%u. The tailnet's policy may not let this "
                 "device reach it.", url->host, (unsigned)url->port);
        break;
    case HERMES_WS_UNREACHABLE:
        snprintf(out, cap, "No route to %s.", url->host);
        break;
    case HERMES_WS_NOT_WEBSOCKET:
        snprintf(out, cap, "%s:%u is not a Hermes bridge (%s).", url->host, (unsigned)url->port,
                 c->error);
        break;
    default:
        snprintf(out, cap, "Could not connect: %s.", c->error);
        break;
    }
    return out;
}

typedef enum {
    RUN_LOST,          /* try again after the backoff */
    RUN_REFUSED,       /* try again in 30 s */
    RUN_INCOMPATIBLE,  /* do not try again */
    RUN_STOPPED,
} run_end_t;

/* One connection, from HELLO to its end. *ready is set once HELLO_ACK came. */
static run_end_t run_session(session_t *s, bool *ready)
{
    char text[384];
    int n = hermes_proto_hello(text, sizeof(text), APP_VERSION);
    if (n < 0 || !hermes_ws_conn_send_text(&s->conn, text, (size_t)n, SEND_MS)) {
        set_link(s, HERMES_LINK_WAITING, "The bridge took the connection but not HELLO.");
        return RUN_LOST;
    }
    set_link(s, HERMES_LINK_HANDSHAKING, "");
    ESP_LOGI(TAG, "connected; HELLO sent");

    bool established = false;
    int64_t opened = now_ms(), heard_at = opened;
    for (;;) {
        if (s->stop) return RUN_STOPPED;

        bool heard = false;
        hermes_ws_event_t event = hermes_ws_conn_poll(&s->conn, POLL_MS, &heard);
        if (heard) heard_at = now_ms();

        if (event == HERMES_WS_TEXT) {
            hermes_frame_info_t info;
            lock();
            hermes_frame_t frame = hermes_proto_read(&s->chat, established,
                                                     (const char *)s->conn.dec.msg,
                                                     s->conn.dec.msg_len, &info);
            switch (frame) {
            case HERMES_FRAME_HELLO_ACK:
                established = true;
                *ready = true;
                s->status.caps = info.caps;
                s->status.link = info.caps.device_chat ? HERMES_LINK_SYNCING : HERMES_LINK_READY;
                s->status.reason[0] = '\0';
                hermes_chat_touch(&s->chat, NULL);
                break;
            case HERMES_FRAME_SYNC_COMPLETE:
                s->status.link = HERMES_LINK_READY;
                hermes_chat_touch(&s->chat, NULL);
                break;
            case HERMES_FRAME_REFUSED:
                s->status.link = HERMES_LINK_REFUSED;
                put_reason(s->status.reason, sizeof(s->status.reason), "", info.message);
                break;
            case HERMES_FRAME_INCOMPATIBLE:
                s->status.link = HERMES_LINK_INCOMPATIBLE;
                put_reason(s->status.reason, sizeof(s->status.reason), "", info.message);
                put_reason(s->status.update, sizeof(s->status.update), "", info.update);
                break;
            case HERMES_FRAME_HELLO_REJECTED:
                s->status.link = HERMES_LINK_WAITING;
                put_reason(s->status.reason, sizeof(s->status.reason), "Hermes refused the session: ",
                           info.message[0] ? info.message : info.code);
                break;
            case HERMES_FRAME_APPLIED:
                if (strcmp(info.type, "CHAT_ACCEPTED") == 0 && s->stop_request[0] &&
                    strcmp(info.request_id, s->stop_request) == 0) {
                    s->chat.awaiting_reply = false;
                    s->chat.streaming = false;
                    s->stop_request[0] = '\0';
                    hermes_chat_touch(&s->chat, &s->chat.transcript_revision);
                }
                break;
            case HERMES_FRAME_INVALID:
                ESP_LOGW(TAG, "a %s frame this device cannot read", info.type[0] ? info.type : "JSON");
                break;
            default:
                break;
            }
            hermes_link_t link = s->status.link;
            uint8_t rows = s->chat.row_count, commands = s->chat.command_count;
            unlock();

            if (frame == HERMES_FRAME_HELLO_ACK) {
                ESP_LOGI(TAG, "session up as %s:%s%s%s%s", s->chat.registration[0] ? s->chat.registration : "?",
                         info.caps.device_chat ? " device_chat" : "",
                         info.caps.chat_history ? " chat_history" : "",
                         info.caps.session_list ? " session_list" : "",
                         info.caps.scheduled_runs ? " scheduled_runs" : "");
            } else if (frame == HERMES_FRAME_SYNC_COMPLETE) {
                ESP_LOGI(TAG, "synced: %u rows, %u commands", rows, commands);
            } else if (frame == HERMES_FRAME_APPLIED && strcmp(info.type, "ERROR") == 0) {
                ESP_LOGW(TAG, "bridge error %s: %s", info.code, info.message);
            } else if (frame == HERMES_FRAME_APPLIED) {
                ESP_LOGD(TAG, "%s", info.type);
            }
            if (link == HERMES_LINK_REFUSED) {
                ESP_LOGW(TAG, "refused: %s", info.message);
                return RUN_REFUSED;
            }
            if (link == HERMES_LINK_INCOMPATIBLE) {
                ESP_LOGW(TAG, "incompatible; update the %s", info.update);
                return RUN_INCOMPATIBLE;
            }
            if (!established && frame == HERMES_FRAME_HELLO_REJECTED) {
                ESP_LOGW(TAG, "HELLO refused: %s %s", info.code, info.message);
                return RUN_LOST;
            }
        } else if (event == HERMES_WS_CLOSE) {
            char why[96];
            snprintf(why, sizeof(why), "The bridge closed the session (%u).",
                     (unsigned)s->conn.dec.close_code);
            ESP_LOGW(TAG, "%s", why);
            set_link(s, HERMES_LINK_WAITING, why);
            return RUN_LOST;
        } else if (event == HERMES_WS_ERROR) {
            char why[128];
            snprintf(why, sizeof(why), "The connection was lost: %s.", s->conn.error);
            ESP_LOGW(TAG, "%s", why);
            set_link(s, HERMES_LINK_WAITING, why);
            return RUN_LOST;
        }

        int64_t now = now_ms();
        if (!established && now - opened > HELLO_MS) {
            set_link(s, HERMES_LINK_WAITING, "The bridge did not answer HELLO.");
            return RUN_LOST;
        }
        if (now - heard_at > SILENCE_MS) {
            ESP_LOGW(TAG, "nothing from the bridge for %d s", SILENCE_MS / 1000);
            set_link(s, HERMES_LINK_WAITING, "The bridge stopped answering.");
            return RUN_LOST;
        }
        if (established && !send_next(s)) {
            char why[128];
            snprintf(why, sizeof(why), "The connection was lost: %s.", s->conn.error);
            set_link(s, HERMES_LINK_WAITING, why);
            return RUN_LOST;
        }
        lock();
        bool busy = hermes_chat_busy(&s->chat);
        unlock();
        keep_awake(s, busy);
    }
}

static void session_task(void *arg)
{
    session_t *s = arg;
    uint32_t backoff_s = 1;
    char url[HERMES_WS_URL_MAX];
    hermes_ws_format_url(&s->url, url, sizeof(url));

    while (!s->stop) {
        set_link(s, HERMES_LINK_CONNECTING, NULL);
        ESP_LOGI(TAG, "connecting to %s", url);
        hermes_ws_open_t open = hermes_ws_conn_open(&s->conn, &s->url, s->rx, sizeof(s->rx),
                                                    CONNECT_MS, &s->stop);
        if (open == HERMES_WS_CANCELLED || s->stop) break;
        if (open != HERMES_WS_OPENED) {
            char why[160];
            open_reason(open, &s->conn, &s->url, why, sizeof(why));
            ESP_LOGW(TAG, "%s", why);
            lock();
            s->status.open = open;
            unlock();
            set_link(s, HERMES_LINK_WAITING, why);
            wait_to_retry(s, backoff_s);
            backoff_s = backoff_s * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : backoff_s * 2;
            continue;
        }

        bool ready = false;
        run_end_t end = run_session(s, &ready);
        hermes_ws_conn_close(&s->conn, end == RUN_STOPPED ? HERMES_WS_CLOSE_GOING_AWAY
                                                          : HERMES_WS_CLOSE_NORMAL);
        forget_outbox(s);
        lock();
        hermes_chat_session_ended(&s->chat);
        s->stop_request[0] = '\0';
        unlock();
        keep_awake(s, false);
        if (ready) backoff_s = 1;

        if (end == RUN_STOPPED) break;
        if (end == RUN_INCOMPATIBLE) {
            while (!s->stop && !s->retry_now) vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }
        if (end == RUN_REFUSED) {
            wait_to_retry(s, REFUSED_RETRY_S);
            continue;
        }
        lock();
        if (s->status.link != HERMES_LINK_WAITING) s->status.link = HERMES_LINK_WAITING;
        unlock();
        ESP_LOGI(TAG, "trying again in %u s", (unsigned)backoff_s);
        wait_to_retry(s, backoff_s);
        backoff_s = backoff_s * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : backoff_s * 2;
    }

    keep_awake(s, false);
#ifndef SIMULATOR_BUILD
    ESP_LOGI(TAG, "session ended; %u bytes of its stack were never used",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
#else
    ESP_LOGI(TAG, "session ended");
#endif
    /* Detached by hermes_bridge_stop() before `stop` was set: nobody else
     * can reach it now. */
    free(s);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* The screens' side                                                    */
/* ------------------------------------------------------------------ */

esp_err_t hermes_bridge_start(const char *url)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    hermes_ws_url_t parsed;
    if (hermes_ws_parse_url(url, &parsed) != NULL) return ESP_ERR_INVALID_ARG;

    lock();
    bool same = s_current && strcmp(s_current->url.host, parsed.host) == 0 &&
                s_current->url.port == parsed.port && strcmp(s_current->url.path, parsed.path) == 0;
    unlock();
    if (same) return ESP_OK;
    hermes_bridge_stop();

    session_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM);
    if (!s) s = malloc(sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;
    memset(s, 0, offsetof(session_t, rx));
    s->url = parsed;
    hermes_chat_init(&s->chat);
    /* Revisions never repeat across sessions, so a screen holding the last
     * session's numbers sees the first change of this one. */
    uint32_t base = ++s_generation << 24;
    s->chat.revision = s->chat.transcript_revision = s->chat.commands_revision = base;
    s->chat.sessions_revision = s->chat.scheduled_revision = base;
    hermes_ws_format_url(&parsed, s->status.url, sizeof(s->status.url));
    s->status.link = HERMES_LINK_CONNECTING;

    lock();
    s_current = s;
    unlock();
    if (xTaskCreate(session_task, "hermes", TASK_STACK, s, TASK_PRIORITY, NULL) != pdPASS) {
        lock();
        s_current = NULL;
        unlock();
        free(s);
        ESP_LOGE(TAG, "no memory for the session's task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void hermes_bridge_stop(void)
{
    if (!s_lock) return;
    lock();
    session_t *s = s_current;
    s_current = NULL;
    if (s) s->stop = true;
    unlock();
}

bool hermes_bridge_running(void)
{
    if (!s_lock) return false;
    lock();
    bool running = s_current != NULL;
    unlock();
    return running;
}

void hermes_bridge_retry_now(void)
{
    if (!s_lock) return;
    lock();
    if (s_current) s_current->retry_now = true;
    unlock();
}

void hermes_bridge_status(hermes_link_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_lock) return;
    lock();
    if (s_current) *out = s_current->status;
    unlock();
}

bool hermes_bridge_snapshot(hermes_chat_t *out, uint32_t *revision)
{
    if (!s_lock) return false;
    bool copied = false;
    lock();
    if (s_current && s_current->chat.revision != *revision) {
        memcpy(out, &s_current->chat, sizeof(*out));
        *revision = s_current->chat.revision;
        copied = true;
    }
    unlock();
    return copied;
}

/* Queues a frame. Called with the lock held. */
static bool enqueue(session_t *s, const char *text, int len, const char **why)
{
    if (len < 0) {
        *why = "That is too long to send.";
        return false;
    }
    for (int i = 0; i < OUTBOX_SLOTS; i++) {
        slot_t *slot = &s->outbox[i];
        if (slot->used) continue;
        memcpy(slot->text, text, (size_t)len + 1);
        slot->len = (size_t)len;
        slot->used = true;
        slot->sending = false;
        slot->sequence = ++s->sequence;
        return true;
    }
    *why = "Still sending what came before.";
    return false;
}

/* The session, locked, if it is up; else NULL and *why. */
static session_t *ready_session(const char **why)
{
    static const char *unused;
    if (!why) why = &unused;
    if (!s_lock) {
        *why = "Hermes is not connected.";
        return NULL;
    }
    lock();
    session_t *s = s_current;
    if (!s || (s->status.link != HERMES_LINK_READY && s->status.link != HERMES_LINK_SYNCING)) {
        unlock();
        *why = "Hermes is not connected.";
        return NULL;
    }
    return s;
}

/* Where the screens' frames are written before they are queued; only the
 * LVGL task writes here, with the lock held. */
static EXT_RAM_BSS_ATTR char s_frame[OUTBOX_BYTES];

bool hermes_bridge_submit(const char *text, const char **why)
{
    const char *p = text;
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    if (!*p) {
        *why = "Nothing to send.";
        return false;
    }
    if (strlen(text) > HERMES_DRAFT_MAX) {
        *why = "That is too long to send.";
        return false;
    }
    session_t *s = ready_session(why);
    if (!s) return false;
    char request[HERMES_CHAT_REQUEST_BYTES];
    new_request_id(request);
    bool queued = enqueue(s, s_frame, hermes_proto_submit(s_frame, sizeof(s_frame), request, text), why);
    if (queued) {
        s->chat.awaiting_reply = true;
        hermes_chat_touch(&s->chat, &s->chat.transcript_revision);
    }
    unlock();
    return queued;
}

bool hermes_bridge_control(const char *action, const char **why)
{
    session_t *s = ready_session(why);
    if (!s) return false;
    char request[HERMES_CHAT_REQUEST_BYTES];
    new_request_id(request);
    bool queued = enqueue(s, s_frame, hermes_proto_control(s_frame, sizeof(s_frame), request, action),
                          why);
    if (queued && strcmp(action, "stop") == 0) memcpy(s->stop_request, request, sizeof(request));
    unlock();
    return queued;
}

bool hermes_bridge_request_sessions(const char **why)
{
    session_t *s = ready_session(why);
    if (!s) return false;
    bool queued = false;
    if (!s->status.caps.session_list) {
        hermes_chat_set_sessions_state(&s->chat, HERMES_LIST_UNAVAILABLE);
        *why = "This Hermes cannot list its sessions.";
    } else {
        queued = enqueue(s, s_frame, hermes_proto_request(s_frame, sizeof(s_frame), "SESSION_LIST_REQUEST"),
                         why);
        if (queued) hermes_chat_set_sessions_state(&s->chat, HERMES_LIST_WAITING);
    }
    unlock();
    return queued;
}

bool hermes_bridge_select_session(const char *session_id, const char **why)
{
    session_t *s = ready_session(why);
    if (!s) return false;
    char request[HERMES_CHAT_REQUEST_BYTES];
    new_request_id(request);
    bool queued = enqueue(s, s_frame,
                          hermes_proto_session_select(s_frame, sizeof(s_frame), request, session_id), why);
    unlock();
    return queued;
}

bool hermes_bridge_request_jobs(const char **why)
{
    session_t *s = ready_session(why);
    if (!s) return false;
    bool queued = false;
    if (!s->status.caps.scheduled_runs) {
        *why = "This Hermes has no Scheduled Runs page.";
    } else {
        queued = enqueue(s, s_frame,
                         hermes_proto_request(s_frame, sizeof(s_frame), "SCHEDULED_JOBS_REQUEST"), why);
        if (queued) hermes_chat_set_jobs_state(&s->chat, HERMES_LIST_WAITING);
    }
    unlock();
    return queued;
}

bool hermes_bridge_request_run(const char *job_id, uint32_t offset, const char **why)
{
    session_t *s = ready_session(why);
    if (!s) return false;
    bool queued = false;
    if (!s->status.caps.scheduled_runs) {
        *why = "This Hermes has no Scheduled Runs page.";
    } else {
        queued = enqueue(s, s_frame,
                         hermes_proto_run_request(s_frame, sizeof(s_frame), job_id, offset), why);
        if (queued) {
            snprintf(s->chat.run.job_id, sizeof(s->chat.run.job_id), "%s", job_id);
            s->chat.run.offset = offset;
            hermes_chat_set_run_state(&s->chat, HERMES_LIST_WAITING);
        }
    }
    unlock();
    return queued;
}
