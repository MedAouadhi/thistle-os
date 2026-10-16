/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Hermes app's Device Chat state. See hermes_chat.h.
 */
#include "hermes/hermes_chat.h"

#include <stdio.h>
#include <string.h>

static bool fits(const char *text, size_t cap)
{
    return text && strlen(text) < cap;
}

static void copy(char *to, size_t cap, const char *from)
{
    size_t n = strlen(from);
    if (n >= cap) n = cap - 1;
    memcpy(to, from, n);
    to[n] = '\0';
}

void hermes_chat_touch(hermes_chat_t *chat, uint32_t *what)
{
    chat->revision++;
    if (what) (*what)++;
}

void hermes_chat_init(hermes_chat_t *chat)
{
    memset(chat, 0, sizeof(*chat));
    /* What the rows show until the bridge says otherwise: Hermes's own
     * live_status default, every row, completed ones collapsed. */
    chat->visibility = HERMES_PROGRESS_ALL;
}

bool hermes_chat_append(hermes_chat_t *chat, const char *id, hermes_role_t role,
                        const char *text, uint64_t position)
{
    if (!id || !id[0] || !fits(id, HERMES_CHAT_ID_BYTES) || !fits(text, HERMES_CHAT_TEXT_BYTES) ||
        role > HERMES_ROLE_ERROR) {
        return false;
    }
    if (chat->row_count == HERMES_CHAT_ROWS) {
        memmove(&chat->rows[0], &chat->rows[1], sizeof(chat->rows[0]) * (HERMES_CHAT_ROWS - 1));
        chat->row_count--;
    }
    hermes_row_t *row = &chat->rows[chat->row_count++];
    memset(row, 0, sizeof(*row));
    copy(row->id, sizeof(row->id), id);
    copy(row->text, sizeof(row->text), text);
    row->role = role;
    row->position = position;
    row->arrival = ++chat->arrivals;
    if (position > chat->acknowledged_position) chat->acknowledged_position = position;
    hermes_chat_touch(chat, &chat->transcript_revision);
    return true;
}

bool hermes_chat_append_notice(hermes_chat_t *chat, hermes_role_t role, const char *text)
{
    if (role != HERMES_ROLE_WARNING && role != HERMES_ROLE_ERROR) return false;
    char id[HERMES_CHAT_ID_BYTES];
    snprintf(id, sizeof(id), "notice-%u", (unsigned)++chat->notice_sequence);
    return hermes_chat_append(chat, id, role, text, 0);
}

void hermes_chat_remove_group(hermes_chat_t *chat, const char *id)
{
    if (!id || !id[0]) return;
    size_t n = strlen(id);
    uint8_t kept = 0;
    for (uint8_t i = 0; i < chat->row_count; i++) {
        const char *candidate = chat->rows[i].id;
        bool member = strcmp(candidate, id) == 0 ||
                      (strncmp(candidate, id, n) == 0 && candidate[n] == '.');
        if (member) continue;
        if (kept != i) chat->rows[kept] = chat->rows[i];
        kept++;
    }
    if (kept == chat->row_count) return;
    memset(&chat->rows[kept], 0, sizeof(chat->rows[0]) * (size_t)(chat->row_count - kept));
    chat->row_count = kept;
    hermes_chat_touch(chat, &chat->transcript_revision);
}

void hermes_chat_reset(hermes_chat_t *chat, const char *conversation_id)
{
    if (conversation_id) copy(chat->conversation_id, sizeof(chat->conversation_id), conversation_id);
    memset(chat->rows, 0, sizeof(chat->rows));
    chat->row_count = 0;
    chat->acknowledged_position = 0;
    memset(chat->activities, 0, sizeof(chat->activities));
    chat->activity_count = 0;
    chat->awaiting_reply = false;
    chat->streaming = false;
    hermes_chat_touch(chat, &chat->transcript_revision);
}

void hermes_chat_session_ended(hermes_chat_t *chat)
{
    chat->syncing = false;
    chat->awaiting_reply = false;
    chat->streaming = false;
    /* A tool that was running is not running for anyone now; its row is
     * gone with the session, as the next sync will not bring it back. */
    memset(chat->activities, 0, sizeof(chat->activities));
    chat->activity_count = 0;
    /* A list being waited on is not coming. An answered one goes too: its
     * ages were measured against a clock that is now stale. */
    if (chat->sessions_state != HERMES_LIST_NOT_ASKED) {
        hermes_chat_set_sessions_state(chat, HERMES_LIST_UNAVAILABLE);
    }
    if (chat->jobs_state == HERMES_LIST_WAITING) {
        hermes_chat_set_jobs_state(chat, HERMES_LIST_UNAVAILABLE);
    }
    if (chat->run.state == HERMES_LIST_WAITING) {
        hermes_chat_set_run_state(chat, HERMES_LIST_UNAVAILABLE);
    }
    hermes_chat_touch(chat, &chat->transcript_revision);
}

/* ------------------------------------------------------------------ */
/* Activity                                                             */
/* ------------------------------------------------------------------ */

/* The oldest completed row goes first, else the oldest. */
static void evict_activity(hermes_chat_t *chat)
{
    uint8_t victim = 0;
    for (uint8_t i = 0; i < chat->activity_count; i++) {
        if (chat->activities[i].state == HERMES_ACTIVITY_COMPLETED) {
            victim = i;
            break;
        }
    }
    memmove(&chat->activities[victim], &chat->activities[victim + 1],
            sizeof(chat->activities[0]) * (size_t)(chat->activity_count - victim - 1));
    chat->activity_count--;
}

bool hermes_chat_record_activity(hermes_chat_t *chat, const char *id, const char *label,
                                 const char *detail, hermes_activity_state_t state)
{
    if (!id || !id[0] || !label || !label[0] || !detail || state > HERMES_ACTIVITY_FAILED ||
        !fits(id, HERMES_CHAT_ACTIVITY_ID_BYTES) || !fits(label, HERMES_CHAT_ACTIVITY_LABEL_BYTES) ||
        !fits(detail, HERMES_CHAT_ACTIVITY_DETAIL_BYTES)) {
        return false;
    }
    hermes_activity_t *row = NULL;
    for (uint8_t i = 0; i < chat->activity_count; i++) {
        if (strcmp(chat->activities[i].id, id) == 0) {
            row = &chat->activities[i];
            break;
        }
    }
    if (!row) {
        if (chat->activity_count == HERMES_CHAT_ACTIVITIES) evict_activity(chat);
        row = &chat->activities[chat->activity_count++];
        memset(row, 0, sizeof(*row));
        copy(row->id, sizeof(row->id), id);
        row->anchor = chat->acknowledged_position;
    }
    copy(row->label, sizeof(row->label), label);
    copy(row->detail, sizeof(row->detail), detail);
    row->state = state;
    /* Every report: a tool finishing is the newest thing on the screen even
     * though its row has not moved since it started. */
    row->arrival = ++chat->arrivals;
    hermes_chat_touch(chat, &chat->transcript_revision);
    return true;
}

hermes_activity_shown_t hermes_chat_activity_shown(const hermes_chat_t *chat, uint8_t index)
{
    if (index >= chat->activity_count) return HERMES_ACTIVITY_HIDDEN;
    hermes_activity_state_t state = chat->activities[index].state;
    switch (chat->visibility) {
    case HERMES_PROGRESS_OFF:
    case HERMES_PROGRESS_LOG:
        return HERMES_ACTIVITY_HIDDEN;
    case HERMES_PROGRESS_NEW:
        if (state != HERMES_ACTIVITY_COMPLETED) return HERMES_ACTIVITY_EXPANDED;
        return index + 1 == chat->activity_count ? HERMES_ACTIVITY_COLLAPSED : HERMES_ACTIVITY_HIDDEN;
    case HERMES_PROGRESS_ALL:
        return state == HERMES_ACTIVITY_COMPLETED ? HERMES_ACTIVITY_COLLAPSED
                                                  : HERMES_ACTIVITY_EXPANDED;
    default:
        return HERMES_ACTIVITY_EXPANDED;
    }
}

bool hermes_chat_activity_running(const hermes_chat_t *chat)
{
    for (uint8_t i = 0; i < chat->activity_count; i++) {
        if (chat->activities[i].state == HERMES_ACTIVITY_RUNNING) return true;
    }
    return false;
}

bool hermes_chat_busy(const hermes_chat_t *chat)
{
    return chat->awaiting_reply || chat->streaming || hermes_chat_activity_running(chat);
}

/* ------------------------------------------------------------------ */
/* Lists                                                                */
/* ------------------------------------------------------------------ */

bool hermes_chat_put_commands(hermes_chat_t *chat, const hermes_command_t *commands,
                              size_t count, bool replace)
{
    size_t already = replace ? 0 : chat->command_count;
    if (already + count > HERMES_CHAT_COMMANDS) return false;
    for (size_t i = 0; i < count; i++) {
        if (!commands[i].name[0] || !commands[i].description[0]) return false;
    }
    if (replace) {
        memset(chat->commands, 0, sizeof(chat->commands));
        chat->command_count = 0;
    }
    memcpy(&chat->commands[chat->command_count], commands, sizeof(*commands) * count);
    chat->command_count = (uint8_t)(chat->command_count + count);
    hermes_chat_touch(chat, &chat->commands_revision);
    return true;
}

bool hermes_chat_put_sessions(hermes_chat_t *chat, const hermes_session_t *sessions,
                              size_t count)
{
    if (count > HERMES_CHAT_SESSIONS) return false;
    for (size_t i = 0; i < count; i++) {
        /* A row is chosen by its id and read by its label. */
        if (!sessions[i].id[0] || !sessions[i].label[0]) return false;
    }
    memset(chat->sessions, 0, sizeof(chat->sessions));
    memcpy(chat->sessions, sessions, sizeof(*sessions) * count);
    chat->session_count = (uint8_t)count;
    chat->sessions_state = HERMES_LIST_LOADED;
    hermes_chat_touch(chat, &chat->sessions_revision);
    return true;
}

void hermes_chat_set_sessions_state(hermes_chat_t *chat, hermes_list_state_t state)
{
    memset(chat->sessions, 0, sizeof(chat->sessions));
    chat->session_count = 0;
    chat->sessions_state = state;
    hermes_chat_touch(chat, &chat->sessions_revision);
}

bool hermes_chat_put_jobs(hermes_chat_t *chat, const hermes_job_t *jobs, size_t count)
{
    if (count > HERMES_SCHEDULED_JOBS) return false;
    for (size_t i = 0; i < count; i++) {
        if (!jobs[i].job_id[0] || !jobs[i].name[0]) return false;
    }
    memset(chat->jobs, 0, sizeof(chat->jobs));
    memcpy(chat->jobs, jobs, sizeof(*jobs) * count);
    chat->job_count = (uint8_t)count;
    chat->jobs_state = HERMES_LIST_LOADED;
    hermes_chat_touch(chat, &chat->scheduled_revision);
    return true;
}

void hermes_chat_set_jobs_state(hermes_chat_t *chat, hermes_list_state_t state)
{
    memset(chat->jobs, 0, sizeof(chat->jobs));
    chat->job_count = 0;
    chat->jobs_state = state;
    hermes_chat_touch(chat, &chat->scheduled_revision);
}

void hermes_chat_set_run_state(hermes_chat_t *chat, hermes_list_state_t state)
{
    char job_id[HERMES_SCHEDULED_JOB_ID_BYTES];
    uint32_t offset = chat->run.offset;
    memcpy(job_id, chat->run.job_id, sizeof(job_id));
    memset(&chat->run, 0, sizeof(chat->run));
    memcpy(chat->run.job_id, job_id, sizeof(job_id));
    chat->run.offset = offset;
    chat->run.state = state;
    hermes_chat_touch(chat, &chat->scheduled_revision);
}
