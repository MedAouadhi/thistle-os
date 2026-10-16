/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Hermes app's Device Chat state.
 *
 * What the bridge has told this device: the transcript, Hermes's tool
 * activity, its commands, this device's Hermes Sessions and its Scheduled
 * Runs. hermes_proto.c applies the bridge's frames to it; the screens draw
 * a copy of it. It follows hermes_esp32's hermes_device_chat.c, whose rules
 * the bridge is written against:
 *
 * - A message longer than one wire part arrives as several rows, "id",
 *   "id.1", "id.2", each drawn on its own. An edit removes the whole group
 *   and the new parts follow.
 * - Notes (this device's own warnings and errors) are rows that move no
 *   position, so they can never move the reconnect cursor.
 * - An activity row is updated in place by id, and is anchored to how far
 *   the transcript had got when it first arrived, so it draws inside the
 *   turn that made it.
 *
 * Every size is in UTF-8 bytes, a NUL included, and matches the bridge's
 * bounds (hermes_esp32's hermes_device_chat.h).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HERMES_CHAT_ROWS              32
#define HERMES_CHAT_ID_BYTES          40
#define HERMES_CHAT_TEXT_BYTES        768
#define HERMES_CHAT_CONVERSATION_BYTES 64
#define HERMES_CHAT_REQUEST_BYTES     33
#define HERMES_CHAT_COMMANDS          64
#define HERMES_CHAT_COMMAND_NAME_BYTES 24
#define HERMES_CHAT_COMMAND_DESCRIPTION_BYTES 64
#define HERMES_CHAT_COMMAND_ARGS_BYTES 32
#define HERMES_CHAT_ACTIVITIES        6
#define HERMES_CHAT_ACTIVITY_ID_BYTES 32
#define HERMES_CHAT_ACTIVITY_LABEL_BYTES 48
#define HERMES_CHAT_ACTIVITY_DETAIL_BYTES 96
#define HERMES_CHAT_SESSIONS          10
#define HERMES_CHAT_SESSION_ID_BYTES  64
#define HERMES_CHAT_SESSION_LABEL_BYTES 64
#define HERMES_CHAT_REGISTRATION_BYTES 64
#define HERMES_SCHEDULED_JOBS         10
#define HERMES_SCHEDULED_JOB_ID_BYTES 64
#define HERMES_SCHEDULED_JOB_NAME_BYTES 64
#define HERMES_SCHEDULED_SCHEDULE_BYTES 48
#define HERMES_SCHEDULED_CAPTURED_BYTES 32
#define HERMES_SCHEDULED_RUN_PARTS    16
#define HERMES_SCHEDULED_PART_BYTES   HERMES_CHAT_TEXT_BYTES

typedef enum {
    HERMES_ROLE_USER,
    HERMES_ROLE_ASSISTANT,
    HERMES_ROLE_PROGRESS,
    HERMES_ROLE_WARNING,
    HERMES_ROLE_ERROR,
} hermes_role_t;

/* Hermes's tool-progress levels, in the order /verbose cycles them. LOG
 * sends tool lines to a file on Hermes's side and stays silent in chat. */
typedef enum {
    HERMES_PROGRESS_OFF,
    HERMES_PROGRESS_NEW,
    HERMES_PROGRESS_ALL,
    HERMES_PROGRESS_VERBOSE,
    HERMES_PROGRESS_LOG,
} hermes_progress_t;

typedef enum {
    HERMES_ACTIVITY_RUNNING,
    HERMES_ACTIVITY_COMPLETED,
    HERMES_ACTIVITY_FAILED,
} hermes_activity_state_t;

typedef enum {
    HERMES_ACTIVITY_HIDDEN,
    HERMES_ACTIVITY_COLLAPSED,  /* the label alone */
    HERMES_ACTIVITY_EXPANDED,   /* the label and the detail */
} hermes_activity_shown_t;

/* What a list the screens ask for has to show. Never asked, asked and
 * waiting, answered (perhaps with nothing), and Hermes unable to answer:
 * the last two must never look alike. */
typedef enum {
    HERMES_LIST_NOT_ASKED,
    HERMES_LIST_WAITING,
    HERMES_LIST_LOADED,
    HERMES_LIST_UNAVAILABLE,
} hermes_list_state_t;

typedef struct {
    char id[HERMES_CHAT_ID_BYTES];
    hermes_role_t role;
    char text[HERMES_CHAT_TEXT_BYTES];
    uint64_t position;
    uint32_t arrival;
} hermes_row_t;

typedef struct {
    char id[HERMES_CHAT_ACTIVITY_ID_BYTES];
    char label[HERMES_CHAT_ACTIVITY_LABEL_BYTES];
    char detail[HERMES_CHAT_ACTIVITY_DETAIL_BYTES];
    hermes_activity_state_t state;
    uint32_t arrival;
    uint64_t anchor;
} hermes_activity_t;

typedef struct {
    char name[HERMES_CHAT_COMMAND_NAME_BYTES];
    char description[HERMES_CHAT_COMMAND_DESCRIPTION_BYTES];
    char args_hint[HERMES_CHAT_COMMAND_ARGS_BYTES];
} hermes_command_t;

typedef struct {
    char id[HERMES_CHAT_SESSION_ID_BYTES];
    char label[HERMES_CHAT_SESSION_LABEL_BYTES];
    uint32_t messages;
    uint32_t age_seconds;
    bool current;
} hermes_session_t;

typedef struct {
    char job_id[HERMES_SCHEDULED_JOB_ID_BYTES];
    char name[HERMES_SCHEDULED_JOB_NAME_BYTES];
    char schedule[HERMES_SCHEDULED_SCHEDULE_BYTES];
    uint32_t age_seconds;
    uint32_t unread;
} hermes_job_t;

/* One Scheduled Run, as SCHEDULED_RUN carried it. */
typedef struct {
    hermes_list_state_t state;
    char job_id[HERMES_SCHEDULED_JOB_ID_BYTES];
    uint32_t offset;
    bool absent;              /* "run": null, an offset past the oldest */
    char captured[HERMES_SCHEDULED_CAPTURED_BYTES];
    uint32_t age_seconds;
    bool truncated;
    bool parts_dropped;
    bool older;
    uint8_t part_count;
    char parts[HERMES_SCHEDULED_RUN_PARTS][HERMES_SCHEDULED_PART_BYTES];
} hermes_run_t;

typedef struct {
    char conversation_id[HERMES_CHAT_CONVERSATION_BYTES];
    char registration[HERMES_CHAT_REGISTRATION_BYTES];  /* the bridge's name for us */
    uint64_t acknowledged_position;
    uint32_t arrivals;
    uint16_t notice_sequence;
    bool history_unavailable;
    /* Between CHAT_SYNC and CHAT_SYNC_COMPLETE: the transcript is being
     * filled in again, and is not worth drawing half done. */
    bool syncing;

    hermes_row_t rows[HERMES_CHAT_ROWS];
    uint8_t row_count;

    hermes_activity_t activities[HERMES_CHAT_ACTIVITIES];
    uint8_t activity_count;
    hermes_progress_t visibility;

    hermes_command_t commands[HERMES_CHAT_COMMANDS];
    uint8_t command_count;

    hermes_session_t sessions[HERMES_CHAT_SESSIONS];
    uint8_t session_count;
    hermes_list_state_t sessions_state;

    hermes_job_t jobs[HERMES_SCHEDULED_JOBS];
    uint8_t job_count;
    hermes_list_state_t jobs_state;
    hermes_run_t run;

    /* The turn: a message sent and not yet answered, or a reply still
     * being edited. With a running activity, Stop is worth showing. */
    bool awaiting_reply;
    bool streaming;

    /* Counts every change, so a screen can tell that it has one to draw;
     * the others count the changes to what they name. */
    uint32_t revision;
    uint32_t transcript_revision;
    uint32_t commands_revision;
    uint32_t sessions_revision;
    uint32_t scheduled_revision;
} hermes_chat_t;

void hermes_chat_init(hermes_chat_t *chat);

/* Adds a row, dropping the oldest when the transcript is full. Refuses an
 * empty id or a field too long. */
bool hermes_chat_append(hermes_chat_t *chat, const char *id, hermes_role_t role,
                        const char *text, uint64_t position);

/* A note of this device's own, which moves no position. */
bool hermes_chat_append_notice(hermes_chat_t *chat, hermes_role_t role, const char *text);

/* Removes the rows of the message `id`: itself and its parts "id.N". */
void hermes_chat_remove_group(hermes_chat_t *chat, const char *id);

/* Empties the transcript and the activity for `conversation_id`, keeping the
 * visibility Hermes last said. */
void hermes_chat_reset(hermes_chat_t *chat, const char *conversation_id);

/* The Bridge Session ended: what it would supply again goes, apart from the
 * transcript, which stays on screen until the next CHAT_SYNC replaces it. */
void hermes_chat_session_ended(hermes_chat_t *chat);

bool hermes_chat_record_activity(hermes_chat_t *chat, const char *id, const char *label,
                                 const char *detail, hermes_activity_state_t state);
hermes_activity_shown_t hermes_chat_activity_shown(const hermes_chat_t *chat, uint8_t index);
bool hermes_chat_activity_running(const hermes_chat_t *chat);

/* Whether a turn is in flight: a message unanswered, a tool running, or a
 * reply still being written. */
bool hermes_chat_busy(const hermes_chat_t *chat);

/* Adds a page of commands, or replaces them all with it. */
bool hermes_chat_put_commands(hermes_chat_t *chat, const hermes_command_t *commands,
                              size_t count, bool replace);

bool hermes_chat_put_sessions(hermes_chat_t *chat, const hermes_session_t *sessions,
                              size_t count);
void hermes_chat_set_sessions_state(hermes_chat_t *chat, hermes_list_state_t state);

bool hermes_chat_put_jobs(hermes_chat_t *chat, const hermes_job_t *jobs, size_t count);
void hermes_chat_set_jobs_state(hermes_chat_t *chat, hermes_list_state_t state);
void hermes_chat_set_run_state(hermes_chat_t *chat, hermes_list_state_t state);

/* Marks a change: every reader that cares re-reads. */
void hermes_chat_touch(hermes_chat_t *chat, uint32_t *what);
