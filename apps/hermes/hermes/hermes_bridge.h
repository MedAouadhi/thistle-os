/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Hermes app's Bridge Session.
 *
 * One WebSocket to the Hermes Device Bridge, kept up by a FreeRTOS task of
 * its own while the app wants it: connected, HELLO said, the transcript
 * synced, frames applied to the chat state as they arrive, and opened again
 * a second after a loss, then up to 30 seconds apart. The screens read a
 * copy of the chat state and queue what they send; they never touch the
 * socket. See .scratch/hermes/spec.md.
 */
#pragma once

#include "hermes/hermes_chat.h"
#include "hermes/hermes_proto.h"
#include "hermes/hermes_ws_conn.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HERMES_LINK_IDLE,          /* not started */
    HERMES_LINK_CONNECTING,    /* finding the bridge and connecting */
    HERMES_LINK_HANDSHAKING,   /* connected; HELLO said, waiting for HELLO_ACK */
    HERMES_LINK_SYNCING,       /* the transcript is arriving */
    HERMES_LINK_READY,
    HERMES_LINK_WAITING,       /* lost, or could not connect: trying again */
    HERMES_LINK_REFUSED,       /* the bridge does not admit this device; tried
                                  again every 30 s, as a tag can be given */
    HERMES_LINK_INCOMPATIBLE,  /* one side needs updating: not tried again */
} hermes_link_t;

typedef struct {
    hermes_link_t link;
    hermes_ws_open_t open;     /* why the last connect failed */
    char reason[160];          /* what went wrong, as a sentence; or "" */
    char update[16];           /* INCOMPATIBLE: "device" or "plugin" */
    uint32_t retry_in_s;       /* WAITING, REFUSED: until the next try */
    hermes_caps_t caps;        /* what the bridge agreed to, once READY */
    char url[HERMES_WS_URL_MAX];
} hermes_link_status_t;

/* Starts a session with the bridge at `url` (hermes_ws_parse_url's form),
 * or moves the one running there. */
esp_err_t hermes_bridge_start(const char *url);

/* Ends the session. Returns at once: the task says goodbye to the bridge
 * and frees the session by itself, within a tenth of a second, or once a
 * name lookup it is waiting on returns. */
void hermes_bridge_stop(void);

bool hermes_bridge_running(void);

/* Tries again now rather than at the end of the wait. */
void hermes_bridge_retry_now(void);

void hermes_bridge_status(hermes_link_status_t *out);

/* Copies the chat state into `out` if it changed since *revision, which it
 * then updates. False when there is nothing new, or no session. */
bool hermes_bridge_snapshot(hermes_chat_t *out, uint32_t *revision);

/* What the screens send. Each is refused, false with *why set, while the
 * session is not up: nothing is queued for later (hermes_esp32 ADR 0027). */
bool hermes_bridge_submit(const char *text, const char **why);
bool hermes_bridge_control(const char *action, const char **why);   /* "stop", "new" */
bool hermes_bridge_request_sessions(const char **why);
bool hermes_bridge_select_session(const char *session_id, const char **why);
bool hermes_bridge_request_jobs(const char **why);
bool hermes_bridge_request_run(const char *job_id, uint32_t offset, const char **why);
