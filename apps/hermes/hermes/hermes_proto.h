/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Hermes Device Bridge's protocol, version 1.
 *
 * The frames this device sends, as JSON, and the bridge's frames read and
 * applied to the chat state. hermes_esp32's protocol/README.md and the
 * plugin's bridge/service.py are the authority; the tests read its
 * fixtures (apps/hermes/tests/).
 */
#pragma once

#include "hermes/hermes_chat.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HERMES_PROTOCOL_VERSION 1

/* The optional capabilities the bridge agreed to. */
typedef struct {
    bool device_chat;
    bool command_descriptors;
    bool structured_progress;
    bool chat_history;
    bool session_list;
    bool scheduled_runs;
} hermes_caps_t;

typedef enum {
    HERMES_FRAME_INVALID,       /* not a frame this device can read */
    HERMES_FRAME_IGNORED,       /* a frame for something this device does
                                   not do */
    HERMES_FRAME_HELLO_ACK,     /* version, caps, registration in info */
    HERMES_FRAME_REFUSED,       /* AUTHENTICATION_FAILED: the node is not
                                   one the bridge admits */
    HERMES_FRAME_INCOMPATIBLE,  /* PROTOCOL_INCOMPATIBLE: info.update says
                                   which side is out of date */
    HERMES_FRAME_HELLO_REJECTED,/* INVALID_HELLO, or another code before
                                   HELLO_ACK */
    HERMES_FRAME_APPLIED,       /* the chat state took it */
    HERMES_FRAME_SYNC_COMPLETE,
} hermes_frame_t;

typedef struct {
    char type[32];
    char code[48];
    char message[192];
    char update[16];            /* "device" or "plugin" */
    char request_id[HERMES_CHAT_REQUEST_BYTES];
    uint16_t version;
    hermes_caps_t caps;
} hermes_frame_info_t;

/* The HELLO, naming this implementation's version. The length, or -1. */
int hermes_proto_hello(char *out, size_t cap, const char *version);

/* Reads one frame from the bridge. Before HELLO_ACK (`established` false)
 * only HELLO_ACK and ERROR are read; after it, everything is applied to
 * `chat`, an ERROR as a note. */
hermes_frame_t hermes_proto_read(hermes_chat_t *chat, bool established, const char *json,
                                 size_t len, hermes_frame_info_t *info);

/* The frames this device sends. Each returns the length, or -1 if it does
 * not fit in `cap`. */
int hermes_proto_submit(char *out, size_t cap, const char *request_id, const char *text);
int hermes_proto_control(char *out, size_t cap, const char *request_id, const char *action);
int hermes_proto_request(char *out, size_t cap, const char *type);
int hermes_proto_session_select(char *out, size_t cap, const char *request_id,
                                const char *session_id);
int hermes_proto_run_request(char *out, size_t cap, const char *job_id, uint32_t offset);

/* Writes `text` as a JSON string, quotes included. The length, or -1. */
int hermes_json_string(char *out, size_t cap, const char *text);
