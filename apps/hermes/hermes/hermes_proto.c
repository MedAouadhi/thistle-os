/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Hermes Device Bridge's protocol. See hermes_proto.h.
 *
 * The bridge writes "type" first, but nothing promises it, so a frame is
 * read twice: once for its type, then again for the fields that type has.
 */
#include "hermes/hermes_proto.h"

#include "podcasts/pod_json.h"

#if __has_include("esp_attr.h")
#include "esp_attr.h"
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

#include <stdio.h>
#include <string.h>

/* Room to read a frame into before it is applied: only the session's task
 * reads frames, and these are too big for internal RAM. */
static EXT_RAM_BSS_ATTR char s_text[HERMES_CHAT_TEXT_BYTES];
static EXT_RAM_BSS_ATTR hermes_command_t s_commands[HERMES_CHAT_COMMANDS];
static EXT_RAM_BSS_ATTR hermes_session_t s_sessions[HERMES_CHAT_SESSIONS];
static EXT_RAM_BSS_ATTR hermes_job_t s_jobs[HERMES_SCHEDULED_JOBS];
static EXT_RAM_BSS_ATTR hermes_run_t s_run;

/* The capabilities asked for. Not spoken_message or pcmu_audio: dictation
 * here is the OS's, and what it writes is sent as typed text. */
static const char CAPABILITIES[] =
    "[\"bridge_session\",\"device_chat\",\"command_descriptors\","
    "\"structured_progress\",\"chat_history\",\"session_list\",\"scheduled_runs\"]";

/* ------------------------------------------------------------------ */
/* Writing                                                              */
/* ------------------------------------------------------------------ */

int hermes_json_string(char *out, size_t cap, const char *text)
{
    size_t n = 0;
#define PUT(c)                         \
    do {                               \
        if (n + 1 >= cap) return -1;   \
        out[n++] = (char)(c);          \
    } while (0)
    PUT('"');
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            PUT('\\');
            PUT(c);
        } else if (c == '\n') {
            PUT('\\');
            PUT('n');
        } else if (c == '\r') {
            PUT('\\');
            PUT('r');
        } else if (c == '\t') {
            PUT('\\');
            PUT('t');
        } else if (c < 0x20) {
            if (n + 6 >= cap) return -1;
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
        } else {
            PUT(c);
        }
    }
    PUT('"');
#undef PUT
    out[n] = '\0';
    return (int)n;
}

/* snprintf's answer as these functions give it: the length, or -1. */
static int fitted(int n, size_t cap)
{
    return n >= 0 && (size_t)n < cap ? n : -1;
}

int hermes_proto_hello(char *out, size_t cap, const char *version)
{
    char quoted[48];
    if (hermes_json_string(quoted, sizeof(quoted), version) < 0) return -1;
    return fitted(snprintf(out, cap,
                           "{\"type\":\"HELLO\",\"min_version\":%d,\"max_version\":%d,"
                           "\"implementation\":{\"name\":\"thistle-os\",\"version\":%s},"
                           "\"capabilities\":%s}",
                           HERMES_PROTOCOL_VERSION, HERMES_PROTOCOL_VERSION, quoted, CAPABILITIES),
                  cap);
}

int hermes_proto_submit(char *out, size_t cap, const char *request_id, const char *text)
{
    int head = snprintf(out, cap, "{\"type\":\"CHAT_SUBMIT\",\"request_id\":\"%s\",\"text\":",
                        request_id);
    if (fitted(head, cap) < 0) return -1;
    int body = hermes_json_string(out + head, cap - (size_t)head, text);
    if (body < 0 || (size_t)(head + body + 1) >= cap) return -1;
    out[head + body] = '}';
    out[head + body + 1] = '\0';
    return head + body + 1;
}

int hermes_proto_control(char *out, size_t cap, const char *request_id, const char *action)
{
    return fitted(snprintf(out, cap,
                           "{\"type\":\"CHAT_CONTROL\",\"request_id\":\"%s\",\"action\":\"%s\"}",
                           request_id, action),
                  cap);
}

int hermes_proto_request(char *out, size_t cap, const char *type)
{
    return fitted(snprintf(out, cap, "{\"type\":\"%s\"}", type), cap);
}

int hermes_proto_session_select(char *out, size_t cap, const char *request_id,
                                const char *session_id)
{
    char quoted[2 * HERMES_CHAT_SESSION_ID_BYTES + 8];
    if (hermes_json_string(quoted, sizeof(quoted), session_id) < 0) return -1;
    return fitted(snprintf(out, cap,
                           "{\"type\":\"SESSION_SELECT\",\"session_id\":%s,\"request_id\":\"%s\"}",
                           quoted, request_id),
                  cap);
}

int hermes_proto_run_request(char *out, size_t cap, const char *job_id, uint32_t offset)
{
    char quoted[2 * HERMES_SCHEDULED_JOB_ID_BYTES + 8];
    if (hermes_json_string(quoted, sizeof(quoted), job_id) < 0) return -1;
    return fitted(snprintf(out, cap,
                           "{\"type\":\"SCHEDULED_RUN_REQUEST\",\"job_id\":%s,\"offset\":%u}",
                           quoted, (unsigned)offset),
                  cap);
}

/* ------------------------------------------------------------------ */
/* Reading                                                              */
/* ------------------------------------------------------------------ */

static bool read_bool(pod_json_t *j)
{
    int64_t v = 0;
    return pod_json_int(j, &v) && v != 0;
}

static uint64_t read_uint(pod_json_t *j)
{
    int64_t v = 0;
    return pod_json_int(j, &v) && v > 0 ? (uint64_t)v : 0;
}

static bool role_from(const char *text, hermes_role_t *role)
{
    static const struct {
        const char *name;
        hermes_role_t role;
    } ROLES[] = {
        { "user", HERMES_ROLE_USER },         { "assistant", HERMES_ROLE_ASSISTANT },
        { "progress", HERMES_ROLE_PROGRESS }, { "warning", HERMES_ROLE_WARNING },
        { "error", HERMES_ROLE_ERROR },
    };
    for (size_t i = 0; i < sizeof(ROLES) / sizeof(ROLES[0]); i++) {
        if (strcmp(text, ROLES[i].name) == 0) {
            *role = ROLES[i].role;
            return true;
        }
    }
    return false;
}

static bool visibility_from(const char *text, hermes_progress_t *out)
{
    static const char *const LEVELS[] = { "off", "new", "all", "verbose", "log" };
    for (size_t i = 0; i < sizeof(LEVELS) / sizeof(LEVELS[0]); i++) {
        if (strcmp(text, LEVELS[i]) == 0) {
            *out = (hermes_progress_t)i;
            return true;
        }
    }
    return false;
}

static bool activity_state_from(const char *text, hermes_activity_state_t *out)
{
    if (strcmp(text, "running") == 0) *out = HERMES_ACTIVITY_RUNNING;
    else if (strcmp(text, "completed") == 0) *out = HERMES_ACTIVITY_COMPLETED;
    else if (strcmp(text, "failed") == 0) *out = HERMES_ACTIVITY_FAILED;
    else return false;
    return true;
}

static void read_type(const char *json, size_t len, char *type, size_t cap)
{
    type[0] = '\0';
    pod_json_t j;
    pod_json_init(&j, json, len);
    if (!pod_json_object(&j)) return;
    char key[16];
    while (pod_json_key(&j, key, sizeof(key))) {
        if (strcmp(key, "type") == 0) {
            pod_json_string(&j, type, cap);
            return;
        }
        pod_json_skip(&j);
    }
}

/* One wire message: {"id","role","text","position",...}. */
static bool read_message(pod_json_t *j, hermes_chat_t *chat, hermes_role_t *role_out)
{
    char *text = s_text;
    char id[HERMES_CHAT_ID_BYTES] = "";
    char role_name[16] = "";
    uint64_t position = 0;
    bool have_text = false, have_position = false;
    text[0] = '\0';
    if (!pod_json_object(j)) return false;
    char key[16];
    while (pod_json_key(j, key, sizeof(key))) {
        if (strcmp(key, "id") == 0) pod_json_string(j, id, sizeof(id));
        else if (strcmp(key, "role") == 0) pod_json_string(j, role_name, sizeof(role_name));
        else if (strcmp(key, "text") == 0) have_text = pod_json_string(j, text, HERMES_CHAT_TEXT_BYTES);
        else if (strcmp(key, "position") == 0) {
            position = read_uint(j);
            have_position = true;
        } else pod_json_skip(j);
    }
    hermes_role_t role;
    if (j->error || !have_text || !have_position || !role_from(role_name, &role)) return false;
    if (role_out) *role_out = role;
    return hermes_chat_append(chat, id, role, text, position);
}

static bool read_commands(pod_json_t *j, hermes_chat_t *chat, bool replace)
{
    hermes_command_t *page = s_commands;
    size_t count = 0;
    if (!pod_json_array(j)) return false;
    while (pod_json_next(j)) {
        if (count == HERMES_CHAT_COMMANDS) return false;
        hermes_command_t *c = &page[count];
        memset(c, 0, sizeof(*c));
        if (!pod_json_object(j)) return false;
        char key[16];
        while (pod_json_key(j, key, sizeof(key))) {
            if (strcmp(key, "name") == 0) pod_json_string(j, c->name, sizeof(c->name));
            else if (strcmp(key, "description") == 0)
                pod_json_string(j, c->description, sizeof(c->description));
            else if (strcmp(key, "args_hint") == 0)
                pod_json_string(j, c->args_hint, sizeof(c->args_hint));
            else pod_json_skip(j);
        }
        count++;
    }
    return !j->error && hermes_chat_put_commands(chat, page, count, replace);
}

static hermes_frame_t read_sync(hermes_chat_t *chat, pod_json_t *j)
{
    char conversation[HERMES_CHAT_CONVERSATION_BYTES] = "";
    char history[16] = "";
    uint64_t position = 0;
    bool have_conversation = false;
    /* Messages and commands come after the reset, which needs the
     * conversation, so they are read on a second pass. */
    const char *start = j->p;
    char key[24];
    while (pod_json_key(j, key, sizeof(key))) {
        if (strcmp(key, "conversation_id") == 0) {
            have_conversation = pod_json_string(j, conversation, sizeof(conversation));
        } else if (strcmp(key, "acknowledged_position") == 0) {
            position = read_uint(j);
        } else if (strcmp(key, "history") == 0) {
            pod_json_string(j, history, sizeof(history));
        } else {
            pod_json_skip(j);
        }
    }
    if (j->error || !have_conversation || !conversation[0]) return HERMES_FRAME_INVALID;

    hermes_chat_reset(chat, conversation);
    chat->acknowledged_position = position;
    chat->history_unavailable = strcmp(history, "unavailable") == 0;
    chat->syncing = true;

    j->p = start;
    while (pod_json_key(j, key, sizeof(key))) {
        if (strcmp(key, "messages") == 0) {
            if (!pod_json_array(j)) return HERMES_FRAME_INVALID;
            while (pod_json_next(j)) {
                if (!read_message(j, chat, NULL)) return HERMES_FRAME_INVALID;
            }
        } else if (strcmp(key, "commands") == 0) {
            if (!read_commands(j, chat, true)) return HERMES_FRAME_INVALID;
        } else {
            pod_json_skip(j);
        }
    }
    return j->error ? HERMES_FRAME_INVALID : HERMES_FRAME_APPLIED;
}

static hermes_frame_t read_chat_message(hermes_chat_t *chat, pod_json_t *j)
{
    const char *message = NULL;
    bool has_final = false, final = true;
    char key[16];
    while (pod_json_key(j, key, sizeof(key))) {
        if (strcmp(key, "message") == 0) {
            message = j->p;
            pod_json_skip(j);
        } else if (strcmp(key, "final") == 0) {
            has_final = true;
            final = read_bool(j);
        } else {
            pod_json_skip(j);
        }
    }
    if (j->error || !message) return HERMES_FRAME_INVALID;
    pod_json_t m;
    pod_json_init(&m, message, (size_t)(j->end - message));
    hermes_role_t role = HERMES_ROLE_USER;
    if (!read_message(&m, chat, &role)) return HERMES_FRAME_INVALID;
    if (role == HERMES_ROLE_ASSISTANT) {
        chat->awaiting_reply = false;
        chat->streaming = has_final && !final;
    }
    return HERMES_FRAME_APPLIED;
}

static hermes_frame_t read_replace(hermes_chat_t *chat, pod_json_t *j)
{
    char id[HERMES_CHAT_ID_BYTES] = "";
    bool final = true;
    char key[16];
    while (pod_json_key(j, key, sizeof(key))) {
        if (strcmp(key, "message_id") == 0) pod_json_string(j, id, sizeof(id));
        else if (strcmp(key, "final") == 0) final = read_bool(j);
        else pod_json_skip(j);
    }
    if (j->error || !id[0]) return HERMES_FRAME_INVALID;
    hermes_chat_remove_group(chat, id);
    chat->streaming = !final;
    return HERMES_FRAME_APPLIED;
}

static hermes_frame_t read_activity(hermes_chat_t *chat, pod_json_t *j)
{
    char visibility[16] = "";
    char id[HERMES_CHAT_ACTIVITY_ID_BYTES] = "";
    char label[HERMES_CHAT_ACTIVITY_LABEL_BYTES] = "";
    char detail[HERMES_CHAT_ACTIVITY_DETAIL_BYTES] = "";
    char state_name[16] = "";
    bool have_activity = false;
    char key[16];
    while (pod_json_key(j, key, sizeof(key))) {
        if (strcmp(key, "visibility") == 0) {
            pod_json_string(j, visibility, sizeof(visibility));
        } else if (strcmp(key, "activity") == 0) {
            if (pod_json_null(j)) continue;
            if (!pod_json_object(j)) return HERMES_FRAME_INVALID;
            have_activity = true;
            while (pod_json_key(j, key, sizeof(key))) {
                if (strcmp(key, "id") == 0) pod_json_string(j, id, sizeof(id));
                else if (strcmp(key, "label") == 0) pod_json_string(j, label, sizeof(label));
                else if (strcmp(key, "detail") == 0) pod_json_string(j, detail, sizeof(detail));
                else if (strcmp(key, "state") == 0) pod_json_string(j, state_name, sizeof(state_name));
                else pod_json_skip(j);
            }
        } else {
            pod_json_skip(j);
        }
    }
    hermes_progress_t level;
    if (j->error || !visibility_from(visibility, &level)) return HERMES_FRAME_INVALID;
    if (chat->visibility != level) {
        chat->visibility = level;
        hermes_chat_touch(chat, &chat->transcript_revision);
    }
    if (!have_activity) return HERMES_FRAME_APPLIED;
    hermes_activity_state_t state;
    if (!activity_state_from(state_name, &state)) return HERMES_FRAME_INVALID;
    return hermes_chat_record_activity(chat, id, label, detail, state) ? HERMES_FRAME_APPLIED
                                                                       : HERMES_FRAME_INVALID;
}

static hermes_frame_t read_session_list(hermes_chat_t *chat, pod_json_t *j)
{
    hermes_session_t *rows = s_sessions;
    size_t count = 0;
    bool have_list = false;
    char key[16];
    while (pod_json_key(j, key, sizeof(key))) {
        if (strcmp(key, "sessions") != 0) {
            pod_json_skip(j);
            continue;
        }
        have_list = true;
        if (!pod_json_array(j)) return HERMES_FRAME_INVALID;
        while (pod_json_next(j)) {
            if (count == HERMES_CHAT_SESSIONS) return HERMES_FRAME_INVALID;
            hermes_session_t *s = &rows[count++];
            memset(s, 0, sizeof(*s));
            if (!pod_json_object(j)) return HERMES_FRAME_INVALID;
            while (pod_json_key(j, key, sizeof(key))) {
                if (strcmp(key, "id") == 0) pod_json_string(j, s->id, sizeof(s->id));
                else if (strcmp(key, "label") == 0) pod_json_string(j, s->label, sizeof(s->label));
                else if (strcmp(key, "messages") == 0) s->messages = (uint32_t)read_uint(j);
                else if (strcmp(key, "age_seconds") == 0) s->age_seconds = (uint32_t)read_uint(j);
                else if (strcmp(key, "current") == 0) s->current = read_bool(j);
                else pod_json_skip(j);
            }
        }
    }
    if (j->error || !have_list) return HERMES_FRAME_INVALID;
    return hermes_chat_put_sessions(chat, rows, count) ? HERMES_FRAME_APPLIED : HERMES_FRAME_INVALID;
}

static hermes_frame_t read_jobs(hermes_chat_t *chat, pod_json_t *j)
{
    hermes_job_t *rows = s_jobs;
    size_t count = 0;
    bool have_list = false;
    char key[16];
    while (pod_json_key(j, key, sizeof(key))) {
        if (strcmp(key, "jobs") != 0) {
            pod_json_skip(j);
            continue;
        }
        have_list = true;
        if (!pod_json_array(j)) return HERMES_FRAME_INVALID;
        while (pod_json_next(j)) {
            if (count == HERMES_SCHEDULED_JOBS) return HERMES_FRAME_INVALID;
            hermes_job_t *r = &rows[count++];
            memset(r, 0, sizeof(*r));
            if (!pod_json_object(j)) return HERMES_FRAME_INVALID;
            while (pod_json_key(j, key, sizeof(key))) {
                if (strcmp(key, "job_id") == 0) pod_json_string(j, r->job_id, sizeof(r->job_id));
                else if (strcmp(key, "name") == 0) pod_json_string(j, r->name, sizeof(r->name));
                else if (strcmp(key, "schedule") == 0)
                    pod_json_string(j, r->schedule, sizeof(r->schedule));
                else if (strcmp(key, "age_seconds") == 0) r->age_seconds = (uint32_t)read_uint(j);
                else if (strcmp(key, "unread") == 0) r->unread = (uint32_t)read_uint(j);
                else pod_json_skip(j);
            }
        }
    }
    if (j->error || !have_list) return HERMES_FRAME_INVALID;
    return hermes_chat_put_jobs(chat, rows, count) ? HERMES_FRAME_APPLIED : HERMES_FRAME_INVALID;
}

static hermes_frame_t read_run(hermes_chat_t *chat, pod_json_t *j)
{
    hermes_run_t *run = &s_run;
    memset(run, 0, sizeof(*run));
    bool have_run = false;
    char key[16];
    while (pod_json_key(j, key, sizeof(key))) {
        if (strcmp(key, "job_id") == 0) {
            pod_json_string(j, run->job_id, sizeof(run->job_id));
        } else if (strcmp(key, "offset") == 0) {
            run->offset = (uint32_t)read_uint(j);
        } else if (strcmp(key, "run") == 0) {
            have_run = true;
            if (pod_json_null(j)) {
                run->absent = true;
                continue;
            }
            if (!pod_json_object(j)) return HERMES_FRAME_INVALID;
            while (pod_json_key(j, key, sizeof(key))) {
                if (strcmp(key, "captured") == 0) {
                    pod_json_string(j, run->captured, sizeof(run->captured));
                } else if (strcmp(key, "age_seconds") == 0) {
                    run->age_seconds = (uint32_t)read_uint(j);
                } else if (strcmp(key, "truncated") == 0) {
                    run->truncated = read_bool(j);
                } else if (strcmp(key, "parts_dropped") == 0) {
                    run->parts_dropped = read_bool(j);
                } else if (strcmp(key, "older") == 0) {
                    run->older = read_bool(j);
                } else if (strcmp(key, "parts") == 0) {
                    if (!pod_json_array(j)) return HERMES_FRAME_INVALID;
                    while (pod_json_next(j)) {
                        if (run->part_count == HERMES_SCHEDULED_RUN_PARTS) return HERMES_FRAME_INVALID;
                        pod_json_string(j, run->parts[run->part_count], HERMES_SCHEDULED_PART_BYTES);
                        run->part_count++;
                    }
                } else {
                    pod_json_skip(j);
                }
            }
        } else {
            pod_json_skip(j);
        }
    }
    if (j->error || !have_run || !run->job_id[0]) return HERMES_FRAME_INVALID;
    /* An answer to a run no longer being read is the bridge's business. */
    if (strcmp(run->job_id, chat->run.job_id) != 0 || run->offset != chat->run.offset) {
        return HERMES_FRAME_IGNORED;
    }
    run->state = HERMES_LIST_LOADED;
    chat->run = *run;
    hermes_chat_touch(chat, &chat->scheduled_revision);
    return HERMES_FRAME_APPLIED;
}

static hermes_frame_t read_error(hermes_chat_t *chat, bool established, pod_json_t *j,
                                 hermes_frame_info_t *info)
{
    char key[16];
    while (pod_json_key(j, key, sizeof(key))) {
        if (strcmp(key, "code") == 0) pod_json_string(j, info->code, sizeof(info->code));
        else if (strcmp(key, "message") == 0) pod_json_string(j, info->message, sizeof(info->message));
        else if (strcmp(key, "update") == 0) pod_json_string(j, info->update, sizeof(info->update));
        else pod_json_skip(j);
    }
    if (j->error) return HERMES_FRAME_INVALID;
    if (strcmp(info->code, "AUTHENTICATION_FAILED") == 0) return HERMES_FRAME_REFUSED;
    if (strcmp(info->code, "PROTOCOL_INCOMPATIBLE") == 0) return HERMES_FRAME_INCOMPATIBLE;
    if (!established) return HERMES_FRAME_HELLO_REJECTED;

    if (strcmp(info->code, "SESSION_LIST_UNAVAILABLE") == 0) {
        hermes_chat_set_sessions_state(chat, HERMES_LIST_UNAVAILABLE);
    } else if (strcmp(info->code, "HERMES_FAILED") == 0 ||
               strcmp(info->code, "HERMES_UNAVAILABLE") == 0 ||
               strcmp(info->code, "INVALID_CHAT_MESSAGE") == 0) {
        chat->awaiting_reply = false;
    }
    /* Every code is a note too, so a failure can be found again after the
     * panel it was for is put away. It moves no position. */
    hermes_chat_append_notice(chat, HERMES_ROLE_ERROR,
                              info->message[0] ? info->message : info->code);
    return HERMES_FRAME_APPLIED;
}

static hermes_frame_t read_hello_ack(pod_json_t *j, hermes_chat_t *chat, hermes_frame_info_t *info)
{
    bool have_version = false, have_caps = false;
    char key[24];
    while (pod_json_key(j, key, sizeof(key))) {
        if (strcmp(key, "version") == 0) {
            info->version = (uint16_t)read_uint(j);
            have_version = true;
        } else if (strcmp(key, "capabilities") == 0) {
            if (!pod_json_array(j)) return HERMES_FRAME_INVALID;
            have_caps = true;
            while (pod_json_next(j)) {
                char name[32];
                pod_json_string(j, name, sizeof(name));
                hermes_caps_t *c = &info->caps;
                if (strcmp(name, "device_chat") == 0) c->device_chat = true;
                else if (strcmp(name, "command_descriptors") == 0) c->command_descriptors = true;
                else if (strcmp(name, "structured_progress") == 0) c->structured_progress = true;
                else if (strcmp(name, "chat_history") == 0) c->chat_history = true;
                else if (strcmp(name, "session_list") == 0) c->session_list = true;
                else if (strcmp(name, "scheduled_runs") == 0) c->scheduled_runs = true;
            }
        } else if (strcmp(key, "registration") == 0) {
            if (!pod_json_object(j)) continue;
            while (pod_json_key(j, key, sizeof(key))) {
                if (strcmp(key, "name") == 0) {
                    pod_json_string(j, chat->registration, sizeof(chat->registration));
                } else {
                    pod_json_skip(j);
                }
            }
        } else {
            pod_json_skip(j);
        }
    }
    if (j->error || !have_version || !have_caps || info->version != HERMES_PROTOCOL_VERSION) {
        return HERMES_FRAME_INVALID;
    }
    return HERMES_FRAME_HELLO_ACK;
}

hermes_frame_t hermes_proto_read(hermes_chat_t *chat, bool established, const char *json,
                                 size_t len, hermes_frame_info_t *info)
{
    memset(info, 0, sizeof(*info));
    read_type(json, len, info->type, sizeof(info->type));
    if (!info->type[0]) return HERMES_FRAME_INVALID;

    pod_json_t j;
    pod_json_init(&j, json, len);
    if (!pod_json_object(&j)) return HERMES_FRAME_INVALID;
    const char *type = info->type;

    if (strcmp(type, "ERROR") == 0) return read_error(chat, established, &j, info);
    if (!established) {
        return strcmp(type, "HELLO_ACK") == 0 ? read_hello_ack(&j, chat, info)
                                              : HERMES_FRAME_INVALID;
    }

    if (strcmp(type, "CHAT_SYNC") == 0) return read_sync(chat, &j);
    if (strcmp(type, "CHAT_MESSAGE") == 0) return read_chat_message(chat, &j);
    if (strcmp(type, "CHAT_MESSAGE_REPLACE") == 0) return read_replace(chat, &j);
    if (strcmp(type, "CHAT_ACTIVITY") == 0) return read_activity(chat, &j);
    if (strcmp(type, "SESSION_LIST") == 0) return read_session_list(chat, &j);
    if (strcmp(type, "SCHEDULED_JOBS") == 0) return read_jobs(chat, &j);
    if (strcmp(type, "SCHEDULED_RUN") == 0) return read_run(chat, &j);

    char key[24];
    if (strcmp(type, "CHAT_COMMANDS") == 0) {
        bool replace = false;
        const char *commands = NULL;
        while (pod_json_key(&j, key, sizeof(key))) {
            if (strcmp(key, "replace") == 0) {
                replace = read_bool(&j);
            } else if (strcmp(key, "commands") == 0) {
                commands = j.p;
                pod_json_skip(&j);
            } else {
                pod_json_skip(&j);
            }
        }
        if (j.error || !commands) return HERMES_FRAME_INVALID;
        pod_json_t c;
        pod_json_init(&c, commands, (size_t)(j.end - commands));
        return read_commands(&c, chat, replace) ? HERMES_FRAME_APPLIED : HERMES_FRAME_INVALID;
    }
    if (strcmp(type, "CHAT_RESET") == 0 || strcmp(type, "CHAT_ACCEPTED") == 0) {
        char conversation[HERMES_CHAT_CONVERSATION_BYTES] = "";
        while (pod_json_key(&j, key, sizeof(key))) {
            if (strcmp(key, "request_id") == 0) {
                pod_json_string(&j, info->request_id, sizeof(info->request_id));
            } else if (strcmp(key, "conversation_id") == 0) {
                pod_json_string(&j, conversation, sizeof(conversation));
            } else {
                pod_json_skip(&j);
            }
        }
        if (j.error) return HERMES_FRAME_INVALID;
        if (strcmp(type, "CHAT_RESET") == 0) {
            if (!conversation[0]) return HERMES_FRAME_INVALID;
            hermes_chat_reset(chat, conversation);
        }
        return HERMES_FRAME_APPLIED;
    }
    if (strcmp(type, "CHAT_SYNC_COMPLETE") == 0) {
        chat->syncing = false;
        hermes_chat_touch(chat, &chat->transcript_revision);
        return HERMES_FRAME_SYNC_COMPLETE;
    }
    if (strcmp(type, "CHAT_COMMANDS_COMPLETE") == 0) return HERMES_FRAME_APPLIED;
    if (strcmp(type, "SCHEDULED_RUNS_UNAVAILABLE") == 0) {
        /* Either request can be answered with it; whichever is waiting is
         * told, and the jobs if neither is. */
        bool told = false;
        if (chat->run.state == HERMES_LIST_WAITING) {
            hermes_chat_set_run_state(chat, HERMES_LIST_UNAVAILABLE);
            told = true;
        }
        if (chat->jobs_state == HERMES_LIST_WAITING || !told) {
            hermes_chat_set_jobs_state(chat, HERMES_LIST_UNAVAILABLE);
        }
        return HERMES_FRAME_APPLIED;
    }
    return HERMES_FRAME_IGNORED;
}
