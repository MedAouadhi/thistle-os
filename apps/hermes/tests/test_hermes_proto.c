// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// The Hermes Device Bridge's protocol and the chat state it fills, with
// frames written the way the bridge writes them (json.dumps with compact
// separators, so anything past ASCII arrives escaped): the handshake and its
// refusals, a sync, replies in parts and their edits, activity rows, the
// command pages, the session list and Scheduled Runs, and what this device
// sends.

#include "hermes/hermes_chat.h"
#include "hermes/hermes_proto.h"
#include "podcasts/pod_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            s_failures++;                                                \
        }                                                                \
    } while (0)

static hermes_chat_t s_chat;
static hermes_frame_info_t s_info;

static hermes_frame_t read_frame(bool established, const char *json)
{
    return hermes_proto_read(&s_chat, established, json, strlen(json), &s_info);
}

static hermes_frame_t frame(const char *json)
{
    return read_frame(true, json);
}

/* ------------------------------------------------------------------ */

static void test_hello(void)
{
    char hello[512];
    int n = hermes_proto_hello(hello, sizeof hello, "0.1.0");
    EXPECT(n > 0 && (size_t)n == strlen(hello));

    /* It reads back as the bridge's parse_hello wants it. */
    pod_json_t j;
    pod_json_init(&j, hello, (size_t)n);
    char key[32], value[64];
    int64_t min = 0, max = 0;
    bool chat = false, spoken = false;
    EXPECT(pod_json_object(&j));
    while (pod_json_key(&j, key, sizeof key)) {
        if (strcmp(key, "type") == 0) {
            pod_json_string(&j, value, sizeof value);
            EXPECT(strcmp(value, "HELLO") == 0);
        } else if (strcmp(key, "min_version") == 0) {
            pod_json_int(&j, &min);
        } else if (strcmp(key, "max_version") == 0) {
            pod_json_int(&j, &max);
        } else if (strcmp(key, "capabilities") == 0) {
            EXPECT(pod_json_array(&j));
            while (pod_json_next(&j)) {
                pod_json_string(&j, value, sizeof value);
                if (strcmp(value, "device_chat") == 0) chat = true;
                if (strcmp(value, "spoken_message") == 0) spoken = true;
            }
        } else {
            pod_json_skip(&j);
        }
    }
    EXPECT(!j.error && min == 1 && max == 1 && chat && !spoken);
    EXPECT(hermes_proto_hello(hello, 40, "0.1.0") == -1);

    hermes_chat_init(&s_chat);
    const char *ack = "{\"type\":\"HELLO_ACK\",\"version\":1,"
                      "\"capabilities\":[\"bridge_session\",\"chat_history\",\"command_descriptors\","
                      "\"device_chat\",\"scheduled_runs\",\"session_list\",\"structured_progress\"],"
                      "\"registration\":{\"id\":\"7d1e\",\"name\":\"thistle-46cf20\",\"state\":\"active\","
                      "\"conversations\":{\"device_chat\":\"078c\"}},\"session_id\":\"ab12\","
                      "\"timezone\":\"CET-1CEST,M3.5.0,M10.5.0/3\"}";
    EXPECT(read_frame(false, ack) == HERMES_FRAME_HELLO_ACK);
    EXPECT(s_info.version == 1 && s_info.caps.device_chat && s_info.caps.session_list);
    EXPECT(s_info.caps.scheduled_runs && s_info.caps.chat_history);
    EXPECT(strcmp(s_chat.registration, "thistle-46cf20") == 0);

    /* An older bridge without the optional ones. */
    EXPECT(read_frame(false, "{\"type\":\"HELLO_ACK\",\"version\":1,\"capabilities\":"
                             "[\"bridge_session\",\"device_chat\"]}") == HERMES_FRAME_HELLO_ACK);
    EXPECT(s_info.caps.device_chat && !s_info.caps.session_list && !s_info.caps.scheduled_runs);

    EXPECT(read_frame(false, "{\"type\":\"HELLO_ACK\",\"capabilities\":[]}") == HERMES_FRAME_INVALID);
    EXPECT(read_frame(false, "{\"type\":\"HELLO_ACK\",\"version\":2,\"capabilities\":[]}") ==
           HERMES_FRAME_INVALID);
    EXPECT(read_frame(false, "{\"type\":\"CHAT_SYNC_COMPLETE\"}") == HERMES_FRAME_INVALID);
    EXPECT(read_frame(false, "not json") == HERMES_FRAME_INVALID);
}

static void test_refusals(void)
{
    hermes_chat_init(&s_chat);
    EXPECT(read_frame(false, "{\"type\":\"ERROR\",\"code\":\"AUTHENTICATION_FAILED\",\"message\":"
                             "\"This tailnet node is not an authorized Hermes device.\"}") ==
           HERMES_FRAME_REFUSED);
    EXPECT(strstr(s_info.message, "not an authorized") != NULL);

    EXPECT(read_frame(false, "{\"type\":\"ERROR\",\"code\":\"PROTOCOL_INCOMPATIBLE\",\"message\":"
                             "\"No common protocol version.\",\"update\":\"device\","
                             "\"supported\":{\"min\":2,\"max\":2}}") == HERMES_FRAME_INCOMPATIBLE);
    EXPECT(strcmp(s_info.update, "device") == 0);

    EXPECT(read_frame(false, "{\"type\":\"ERROR\",\"code\":\"INVALID_HELLO\",\"message\":"
                             "\"capabilities must be an array\"}") == HERMES_FRAME_HELLO_REJECTED);
    EXPECT(strcmp(s_info.code, "INVALID_HELLO") == 0);
    /* None of them is a note in a transcript there is no session for. */
    EXPECT(s_chat.row_count == 0);
}

static void test_sync(void)
{
    hermes_chat_init(&s_chat);
    hermes_chat_append(&s_chat, "stale", HERMES_ROLE_USER, "from the last session", 3);

    EXPECT(frame("{\"type\":\"CHAT_SYNC\",\"conversation_id\":\"078c84476ae445b5\","
                 "\"acknowledged_position\":4,\"history\":\"loaded\",\"messages\":[],"
                 "\"commands\":[]}") == HERMES_FRAME_APPLIED);
    EXPECT(s_chat.syncing && s_chat.row_count == 0 && !s_chat.history_unavailable);
    EXPECT(strcmp(s_chat.conversation_id, "078c84476ae445b5") == 0);
    EXPECT(s_chat.acknowledged_position == 4);

    EXPECT(frame("{\"type\":\"CHAT_MESSAGE\",\"message\":{\"id\":\"h1\",\"role\":\"user\","
                 "\"text\":\"\\u0645\\u0631\\u062d\\u0628\\u0627\",\"position\":1,\"part\":0,\"parts\":1}}") ==
           HERMES_FRAME_APPLIED);
    EXPECT(frame("{\"type\":\"CHAT_MESSAGE\",\"message\":{\"id\":\"h2\",\"role\":\"assistant\","
                 "\"text\":\"Hi \\ud83d\\ude00 **there**\",\"position\":2,\"part\":0,\"parts\":1}}") ==
           HERMES_FRAME_APPLIED);
    EXPECT(s_chat.row_count == 2);
    EXPECT(strcmp(s_chat.rows[0].text, "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7") == 0);
    EXPECT(strcmp(s_chat.rows[1].text, "Hi \xF0\x9F\x98\x80 **there**") == 0);
    EXPECT(s_chat.rows[1].role == HERMES_ROLE_ASSISTANT);

    /* Two pages of commands, the first replacing what there was. */
    EXPECT(frame("{\"type\":\"CHAT_COMMANDS\",\"replace\":true,\"commands\":["
                 "{\"name\":\"new\",\"description\":\"Start a new conversation\",\"args_hint\":\"\"},"
                 "{\"name\":\"model\",\"description\":\"Switch model\",\"args_hint\":\"[name]\"}]}") ==
           HERMES_FRAME_APPLIED);
    EXPECT(frame("{\"type\":\"CHAT_COMMANDS\",\"replace\":false,\"commands\":["
                 "{\"name\":\"fast\",\"description\":\"Faster \\u2014 cheaper\"}]}") ==
           HERMES_FRAME_APPLIED);
    EXPECT(s_chat.command_count == 3 && strcmp(s_chat.commands[1].args_hint, "[name]") == 0);
    EXPECT(strcmp(s_chat.commands[2].description, "Faster \xE2\x80\x94 cheaper") == 0);
    EXPECT(frame("{\"type\":\"CHAT_COMMANDS\",\"replace\":true,\"commands\":["
                 "{\"name\":\"\",\"description\":\"no name\"}]}") == HERMES_FRAME_INVALID);
    EXPECT(s_chat.command_count == 3);

    EXPECT(frame("{\"type\":\"CHAT_SYNC_COMPLETE\"}") == HERMES_FRAME_SYNC_COMPLETE);
    EXPECT(!s_chat.syncing);

    /* History Hermes could not supply is said, not shown as empty. */
    EXPECT(frame("{\"type\":\"CHAT_SYNC\",\"conversation_id\":\"c2\",\"acknowledged_position\":0,"
                 "\"history\":\"unavailable\",\"messages\":[],\"commands\":[]}") == HERMES_FRAME_APPLIED);
    EXPECT(s_chat.history_unavailable && s_chat.command_count == 0);

    EXPECT(frame("{\"type\":\"CHAT_SYNC\",\"acknowledged_position\":0,\"messages\":[]}") ==
           HERMES_FRAME_INVALID);
}

static void test_turn(void)
{
    hermes_chat_init(&s_chat);
    frame("{\"type\":\"CHAT_SYNC\",\"conversation_id\":\"c\",\"acknowledged_position\":0,"
          "\"history\":\"loaded\",\"messages\":[],\"commands\":[]}");
    frame("{\"type\":\"CHAT_SYNC_COMPLETE\"}");

    /* Sent, accepted, echoed. */
    s_chat.awaiting_reply = true;
    EXPECT(frame("{\"type\":\"CHAT_ACCEPTED\",\"request_id\":\"r1\"}") == HERMES_FRAME_APPLIED);
    EXPECT(strcmp(s_info.request_id, "r1") == 0);
    frame("{\"type\":\"CHAT_MESSAGE\",\"message\":{\"id\":\"r1\",\"role\":\"user\",\"text\":\"Weather?\","
          "\"position\":1,\"part\":0,\"parts\":1}}");
    EXPECT(s_chat.awaiting_reply && hermes_chat_busy(&s_chat));

    /* A tool runs, anchored after the question. */
    EXPECT(frame("{\"type\":\"CHAT_ACTIVITY\",\"visibility\":\"all\",\"activity\":{\"id\":\"a1\","
                 "\"label\":\"Searching the web\",\"detail\":\"oslo weather\",\"state\":\"running\"}}") ==
           HERMES_FRAME_APPLIED);
    EXPECT(s_chat.activity_count == 1 && s_chat.activities[0].anchor == 1);
    EXPECT(hermes_chat_activity_shown(&s_chat, 0) == HERMES_ACTIVITY_EXPANDED);

    /* The first answer stops the wait; the tool is still running. */
    frame("{\"type\":\"CHAT_MESSAGE\",\"message\":{\"id\":\"m2\",\"role\":\"assistant\","
          "\"text\":\"Looking...\",\"position\":2,\"part\":0,\"parts\":1}}");
    EXPECT(!s_chat.awaiting_reply && hermes_chat_busy(&s_chat));

    /* Updated in place, and collapsed once done. */
    frame("{\"type\":\"CHAT_ACTIVITY\",\"visibility\":\"all\",\"activity\":{\"id\":\"a1\","
          "\"label\":\"Searched the web\",\"detail\":\"\",\"state\":\"completed\"}}");
    EXPECT(s_chat.activity_count == 1 && strcmp(s_chat.activities[0].label, "Searched the web") == 0);
    EXPECT(s_chat.activities[0].anchor == 1);
    EXPECT(hermes_chat_activity_shown(&s_chat, 0) == HERMES_ACTIVITY_COLLAPSED);
    EXPECT(!hermes_chat_busy(&s_chat));

    /* A level with no rows, and a frame with only the level. */
    EXPECT(frame("{\"type\":\"CHAT_ACTIVITY\",\"visibility\":\"off\",\"activity\":null}") ==
           HERMES_FRAME_APPLIED);
    EXPECT(s_chat.visibility == HERMES_PROGRESS_OFF);
    EXPECT(hermes_chat_activity_shown(&s_chat, 0) == HERMES_ACTIVITY_HIDDEN);
    EXPECT(frame("{\"type\":\"CHAT_ACTIVITY\",\"visibility\":\"loud\"}") == HERMES_FRAME_INVALID);

    /* An edit: the message's parts go, and its new parts follow. */
    frame("{\"type\":\"CHAT_MESSAGE\",\"message\":{\"id\":\"m3\",\"role\":\"assistant\","
          "\"text\":\"part one\",\"position\":3,\"part\":0,\"parts\":2}}");
    frame("{\"type\":\"CHAT_MESSAGE\",\"message\":{\"id\":\"m3.1\",\"role\":\"assistant\","
          "\"text\":\"part two\",\"position\":3,\"part\":1,\"parts\":2}}");
    frame("{\"type\":\"CHAT_MESSAGE\",\"message\":{\"id\":\"m33\",\"role\":\"assistant\","
          "\"text\":\"another message\",\"position\":4,\"part\":0,\"parts\":1}}");
    EXPECT(s_chat.row_count == 5);
    EXPECT(frame("{\"type\":\"CHAT_MESSAGE_REPLACE\",\"message_id\":\"m3\",\"final\":false}") ==
           HERMES_FRAME_APPLIED);
    EXPECT(s_chat.row_count == 3 && strcmp(s_chat.rows[2].id, "m33") == 0);
    EXPECT(s_chat.streaming && hermes_chat_busy(&s_chat));
    frame("{\"type\":\"CHAT_MESSAGE\",\"message\":{\"id\":\"m3\",\"role\":\"assistant\","
          "\"text\":\"rewritten\",\"position\":3,\"part\":0,\"parts\":1},\"final\":true}");
    EXPECT(!s_chat.streaming && s_chat.row_count == 4);

    /* An error is a note: it moves no position. */
    uint64_t at = s_chat.acknowledged_position;
    s_chat.awaiting_reply = true;
    EXPECT(frame("{\"type\":\"ERROR\",\"code\":\"HERMES_FAILED\",\"message\":"
                 "\"Hermes rejected the message: RuntimeError.\"}") == HERMES_FRAME_APPLIED);
    EXPECT(s_chat.row_count == 5 && s_chat.rows[4].role == HERMES_ROLE_ERROR);
    EXPECT(s_chat.rows[4].position == 0 && s_chat.acknowledged_position == at);
    EXPECT(!s_chat.awaiting_reply);

    /* New conversation. */
    EXPECT(frame("{\"type\":\"CHAT_RESET\",\"request_id\":\"r9\",\"conversation_id\":\"c\"}") ==
           HERMES_FRAME_APPLIED);
    EXPECT(s_chat.row_count == 0 && s_chat.activity_count == 0 && strcmp(s_info.request_id, "r9") == 0);
    EXPECT(s_chat.visibility == HERMES_PROGRESS_OFF);

    EXPECT(frame("{\"type\":\"CHAT_MESSAGE\",\"message\":{\"id\":\"x\",\"role\":\"robot\","
                 "\"text\":\"?\",\"position\":1}}") == HERMES_FRAME_INVALID);
    EXPECT(frame("{\"type\":\"CHAT_MESSAGE\",\"message\":{\"id\":\"x\",\"role\":\"user\","
                 "\"position\":1}}") == HERMES_FRAME_INVALID);
    EXPECT(frame("{\"type\":\"SPOKEN_RESULT\",\"operation_id\":\"aabbccdd11223344\","
                 "\"status\":\"submitted\"}") == HERMES_FRAME_IGNORED);
}

static void test_sessions(void)
{
    hermes_chat_init(&s_chat);
    hermes_chat_set_sessions_state(&s_chat, HERMES_LIST_WAITING);
    EXPECT(frame("{\"type\":\"SESSION_LIST\",\"sessions\":[{\"id\":\"20261016_093000_ab12\","
                 "\"label\":\"Electricity prices\",\"messages\":14,\"age_seconds\":120,\"current\":true},"
                 "{\"id\":\"20261015_180000_cd34\",\"label\":\"Oct 15 18:00\",\"messages\":3,"
                 "\"age_seconds\":55000,\"current\":false}]}") == HERMES_FRAME_APPLIED);
    EXPECT(s_chat.sessions_state == HERMES_LIST_LOADED && s_chat.session_count == 2);
    EXPECT(s_chat.sessions[0].current && s_chat.sessions[1].messages == 3);
    EXPECT(s_chat.sessions[1].age_seconds == 55000);

    /* No conversations at all is an answer too. */
    EXPECT(frame("{\"type\":\"SESSION_LIST\",\"sessions\":[]}") == HERMES_FRAME_APPLIED);
    EXPECT(s_chat.sessions_state == HERMES_LIST_LOADED && s_chat.session_count == 0);

    hermes_chat_set_sessions_state(&s_chat, HERMES_LIST_WAITING);
    EXPECT(frame("{\"type\":\"ERROR\",\"code\":\"SESSION_LIST_UNAVAILABLE\",\"message\":"
                 "\"Hermes could not list this device's conversations.\"}") == HERMES_FRAME_APPLIED);
    EXPECT(s_chat.sessions_state == HERMES_LIST_UNAVAILABLE && s_chat.row_count == 1);

    /* A session ending takes the answer and a wait with it. */
    hermes_chat_set_sessions_state(&s_chat, HERMES_LIST_WAITING);
    hermes_chat_session_ended(&s_chat);
    EXPECT(s_chat.sessions_state == HERMES_LIST_UNAVAILABLE);
    EXPECT(s_chat.row_count == 1);
}

static void test_scheduled(void)
{
    hermes_chat_init(&s_chat);
    hermes_chat_set_jobs_state(&s_chat, HERMES_LIST_WAITING);
    /* protocol/fixtures/scheduled-jobs.json */
    EXPECT(frame("{\"type\":\"SCHEDULED_JOBS\",\"jobs\":[{\"job_id\":\"b7c2d4e91a05\","
                 "\"name\":\"Blog monitor\",\"schedule\":\"every 6 hours\",\"age_seconds\":1380,"
                 "\"unread\":1},{\"job_id\":\"a3f19c0e77b4\",\"name\":\"Morning price summary\","
                 "\"schedule\":\"every day at 06:30\",\"age_seconds\":19800,\"unread\":2}]}") ==
           HERMES_FRAME_APPLIED);
    EXPECT(s_chat.jobs_state == HERMES_LIST_LOADED && s_chat.job_count == 2);
    EXPECT(s_chat.jobs[1].unread == 2 && strcmp(s_chat.jobs[1].schedule, "every day at 06:30") == 0);

    /* protocol/fixtures/scheduled-run.json */
    strcpy(s_chat.run.job_id, "a3f19c0e77b4");
    s_chat.run.offset = 0;
    hermes_chat_set_run_state(&s_chat, HERMES_LIST_WAITING);
    const char *run = "{\"type\":\"SCHEDULED_RUN\",\"job_id\":\"a3f19c0e77b4\",\"offset\":0,\"run\":{"
                      "\"captured\":\"2026-08-21 06:30\",\"age_seconds\":19800,\"truncated\":false,"
                      "\"parts_dropped\":false,\"older\":true,\"parts\":["
                      "\"## Morning price summary\\n\\n| Hour | Price | Trend |\\n| --- | --- | --- |\\n"
                      "| 06:00 | 6.20 c/kWh | \\u25b2 |\","
                      "\"| Hour | Price | Trend |\\n| --- | --- | --- |\\n| 08:00 | 5.90 c/kWh | \\u25bc |\"]}}";
    EXPECT(frame(run) == HERMES_FRAME_APPLIED);
    EXPECT(s_chat.run.state == HERMES_LIST_LOADED && s_chat.run.part_count == 2);
    EXPECT(s_chat.run.older && !s_chat.run.absent && strcmp(s_chat.run.captured, "2026-08-21 06:30") == 0);
    EXPECT(strstr(s_chat.run.parts[1], "\xE2\x96\xBC") != NULL);

    /* An answer to a run nobody is reading now. */
    s_chat.run.offset = 1;
    hermes_chat_set_run_state(&s_chat, HERMES_LIST_WAITING);
    EXPECT(frame(run) == HERMES_FRAME_IGNORED);
    EXPECT(s_chat.run.state == HERMES_LIST_WAITING);

    /* Past the oldest is an absence, not an error. */
    s_chat.run.offset = 20;
    EXPECT(frame("{\"type\":\"SCHEDULED_RUN\",\"job_id\":\"a3f19c0e77b4\",\"offset\":20,\"run\":null}") ==
           HERMES_FRAME_APPLIED);
    EXPECT(s_chat.run.state == HERMES_LIST_LOADED && s_chat.run.absent);

    hermes_chat_set_run_state(&s_chat, HERMES_LIST_WAITING);
    EXPECT(frame("{\"type\":\"SCHEDULED_RUNS_UNAVAILABLE\",\"message\":\"Hermes could not read this "
                 "Scheduled Run.\"}") == HERMES_FRAME_APPLIED);
    EXPECT(s_chat.run.state == HERMES_LIST_UNAVAILABLE && s_chat.jobs_state == HERMES_LIST_LOADED);
    hermes_chat_set_jobs_state(&s_chat, HERMES_LIST_WAITING);
    frame("{\"type\":\"SCHEDULED_RUNS_UNAVAILABLE\",\"message\":\"x\"}");
    EXPECT(s_chat.jobs_state == HERMES_LIST_UNAVAILABLE);
}

static void test_model(void)
{
    hermes_chat_init(&s_chat);
    char id[16];
    for (int i = 0; i < HERMES_CHAT_ROWS + 3; i++) {
        snprintf(id, sizeof id, "m%d", i);
        EXPECT(hermes_chat_append(&s_chat, id, HERMES_ROLE_ASSISTANT, "x", (uint64_t)i + 1));
    }
    EXPECT(s_chat.row_count == HERMES_CHAT_ROWS && strcmp(s_chat.rows[0].id, "m3") == 0);
    EXPECT(s_chat.acknowledged_position == HERMES_CHAT_ROWS + 3);

    char long_text[HERMES_CHAT_TEXT_BYTES + 1];
    memset(long_text, 'a', sizeof long_text - 1);
    long_text[sizeof long_text - 1] = '\0';
    EXPECT(!hermes_chat_append(&s_chat, "big", HERMES_ROLE_USER, long_text, 1));
    EXPECT(!hermes_chat_append(&s_chat, "", HERMES_ROLE_USER, "x", 1));

    /* Notes number themselves, so the same one can be shown twice. */
    hermes_chat_init(&s_chat);
    hermes_chat_append_notice(&s_chat, HERMES_ROLE_WARNING, "same");
    hermes_chat_append_notice(&s_chat, HERMES_ROLE_WARNING, "same");
    EXPECT(s_chat.row_count == 2 && strcmp(s_chat.rows[0].id, s_chat.rows[1].id) != 0);
    EXPECT(!hermes_chat_append_notice(&s_chat, HERMES_ROLE_USER, "not a note"));

    /* Full of activity, a completed row goes before a running one. */
    hermes_chat_init(&s_chat);
    for (int i = 0; i < HERMES_CHAT_ACTIVITIES; i++) {
        snprintf(id, sizeof id, "a%d", i);
        hermes_chat_record_activity(&s_chat, id, "tool", "",
                                    i == 2 ? HERMES_ACTIVITY_COMPLETED : HERMES_ACTIVITY_RUNNING);
    }
    hermes_chat_record_activity(&s_chat, "a9", "tool", "", HERMES_ACTIVITY_RUNNING);
    EXPECT(s_chat.activity_count == HERMES_CHAT_ACTIVITIES);
    bool kept_completed = false;
    for (int i = 0; i < s_chat.activity_count; i++) {
        if (strcmp(s_chat.activities[i].id, "a2") == 0) kept_completed = true;
    }
    EXPECT(!kept_completed && strcmp(s_chat.activities[HERMES_CHAT_ACTIVITIES - 1].id, "a9") == 0);
    EXPECT(!hermes_chat_record_activity(&s_chat, "a10", "", "", HERMES_ACTIVITY_RUNNING));

    /* "new": running and failed rows, and only the newest completed one. */
    hermes_chat_init(&s_chat);
    s_chat.visibility = HERMES_PROGRESS_NEW;
    hermes_chat_record_activity(&s_chat, "a", "one", "", HERMES_ACTIVITY_COMPLETED);
    hermes_chat_record_activity(&s_chat, "b", "two", "", HERMES_ACTIVITY_FAILED);
    hermes_chat_record_activity(&s_chat, "c", "three", "", HERMES_ACTIVITY_COMPLETED);
    EXPECT(hermes_chat_activity_shown(&s_chat, 0) == HERMES_ACTIVITY_HIDDEN);
    EXPECT(hermes_chat_activity_shown(&s_chat, 1) == HERMES_ACTIVITY_EXPANDED);
    EXPECT(hermes_chat_activity_shown(&s_chat, 2) == HERMES_ACTIVITY_COLLAPSED);
    s_chat.visibility = HERMES_PROGRESS_VERBOSE;
    EXPECT(hermes_chat_activity_shown(&s_chat, 0) == HERMES_ACTIVITY_EXPANDED);
    s_chat.visibility = HERMES_PROGRESS_LOG;
    EXPECT(hermes_chat_activity_shown(&s_chat, 1) == HERMES_ACTIVITY_HIDDEN);
}

static void test_sending(void)
{
    char out[256];
    int n = hermes_proto_submit(out, sizeof out, "a1b2", "Say \"hi\"\n\tto \xD8\xA8\x01 now\\");
    EXPECT(n > 0 && (size_t)n == strlen(out));
    EXPECT(strcmp(out, "{\"type\":\"CHAT_SUBMIT\",\"request_id\":\"a1b2\",\"text\":"
                       "\"Say \\\"hi\\\"\\n\\tto \xD8\xA8\\u0001 now\\\\\"}") == 0);
    EXPECT(hermes_proto_submit(out, 60, "a1b2", "a long enough text to overflow the buffer") == -1);

    n = hermes_proto_control(out, sizeof out, "c3", "stop");
    EXPECT(n > 0 && strcmp(out, "{\"type\":\"CHAT_CONTROL\",\"request_id\":\"c3\",\"action\":\"stop\"}") == 0);
    n = hermes_proto_request(out, sizeof out, "SESSION_LIST_REQUEST");
    EXPECT(n > 0 && strcmp(out, "{\"type\":\"SESSION_LIST_REQUEST\"}") == 0);
    n = hermes_proto_session_select(out, sizeof out, "s1", "20261016_093000_ab12");
    EXPECT(n > 0 && strcmp(out, "{\"type\":\"SESSION_SELECT\",\"session_id\":\"20261016_093000_ab12\","
                                "\"request_id\":\"s1\"}") == 0);
    n = hermes_proto_run_request(out, sizeof out, "a3f19c0e77b4", 2);
    EXPECT(n > 0 && strcmp(out, "{\"type\":\"SCHEDULED_RUN_REQUEST\",\"job_id\":\"a3f19c0e77b4\","
                                "\"offset\":2}") == 0);
}

int main(void)
{
    test_hello();
    test_refusals();
    test_sync();
    test_turn();
    test_sessions();
    test_scheduled();
    test_model();
    test_sending();
    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_hermes_proto: all checks passed\n");
    return 0;
}
