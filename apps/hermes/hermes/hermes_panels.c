/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Hermes app's panels, over the transcript.
 *
 * - The drawer: New conversation; this device's Hermes Sessions, asked for
 *   as it opens; Scheduled Runs; the bridge's address.
 * - The Command panel: the commands the bridge sent. Choosing one writes it
 *   into the draft and sends nothing.
 * - Scheduled Runs: the jobs that have delivered something, then one job's
 *   runs, newest first, one at a time.
 * - The bridge's address, with the tailnet's peers to pick from.
 *
 * A list the bridge is asked for has four states, drawn as four different
 * sentences, as hermes_esp32's are: not asked (with why not), waiting, an
 * answer (which may be empty), and Hermes unable to answer.
 */
#include "hermes/hermes_ui.h"
#include "hermes/hermes_config.h"
#include "hermes/hermes_glyphs.h"
#include "hermes/hermes_view.h"
#include "hermes/hermes_ws.h"

#include "thistle/tailnet.h"
#include "ui/toast.h"

#include "esp_attr.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "hermes_ui";

#define PAD          6
#define ROWS         16
#define PEERS        12
#define ARENA_BYTES  (HERMES_SCHEDULED_RUN_PARTS * HERMES_SCHEDULED_PART_BYTES + 1024)

static EXT_RAM_BSS_ATTR struct {
    lv_obj_t *root;
    int32_t top;
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *body;
    lv_obj_t *first;
    hermes_panel_t kind;
    uint32_t drawn;
    hermes_arena_t arena;
    const char *why;
    /* What a row's index stands for, as it was when it was drawn. */
    char ids[ROWS][HERMES_SCHEDULED_JOB_ID_BYTES];
    char names[ROWS][HERMES_SCHEDULED_JOB_NAME_BYTES];
    char job_id[HERMES_SCHEDULED_JOB_ID_BYTES];
    char job_name[HERMES_SCHEDULED_JOB_NAME_BYTES];
    uint32_t offset;
    lv_obj_t *url;
    lv_obj_t *url_error;
    tailnet_peer_t peers[PEERS];
} s_p;

static void draw(void);

/* ------------------------------------------------------------------ */
/* Pieces                                                               */
/* ------------------------------------------------------------------ */

static lv_obj_t *note(lv_obj_t *parent, const char *text)
{
    const hermes_view_styles_t *st = hermes_view_styles();
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_add_style(label, (lv_style_t *)&st->secondary, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_text(label, text);
    return label;
}

static void focusable(lv_obj_t *obj)
{
    hermes_ui_add_focus(obj);
    if (!s_p.first) s_p.first = obj;
}

static lv_obj_t *plain_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, intptr_t data)
{
    const hermes_view_styles_t *st = hermes_view_styles();
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, (lv_style_t *)&st->button, LV_PART_MAIN);
    lv_obj_add_style(btn, (lv_style_t *)&st->button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(btn, (lv_style_t *)&st->focus, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)data);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    focusable(btn);
    return btn;
}

/* A full-width row: a title, and a line under it. An inert row is drawn
 * marked and cannot be chosen. */
static lv_obj_t *row(lv_obj_t *parent, const char *title, const char *subtitle, lv_event_cb_t cb,
                     intptr_t index, bool inert)
{
    const hermes_view_styles_t *st = hermes_view_styles();
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, (lv_style_t *)&st->button, LV_PART_MAIN);
    lv_obj_add_style(btn, (lv_style_t *)&st->focus, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_size(btn, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    if (inert) {
        lv_obj_add_style(btn, (lv_style_t *)&st->button_inert, LV_PART_MAIN);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_style(btn, (lv_style_t *)&st->button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)index);
        focusable(btn);
    }
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    char drawn[HERMES_CHAT_SESSION_LABEL_BYTES * 2];
    hermes_glyphs(title, strlen(title), drawn, sizeof(drawn));
    lv_label_set_text(label, drawn);
    if (subtitle && subtitle[0]) {
        lv_obj_t *sub = lv_label_create(btn);
        lv_obj_add_style(sub, (lv_style_t *)&st->secondary, LV_PART_MAIN);
        lv_obj_add_style(sub, (lv_style_t *)&st->button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(sub, LV_PCT(100));
        hermes_glyphs(subtitle, strlen(subtitle), drawn, sizeof(drawn));
        lv_label_set_text(sub, drawn);
    }
    return btn;
}

static const char *list_sentence(hermes_list_state_t state, const char *waiting,
                                 const char *unavailable)
{
    if (state == HERMES_LIST_WAITING) return waiting;
    if (state == HERMES_LIST_UNAVAILABLE) return unavailable;
    return s_p.why ? s_p.why : "Hermes is not connected.";
}

/* ------------------------------------------------------------------ */
/* The drawer                                                           */
/* ------------------------------------------------------------------ */

static void new_conversation_cb(lv_event_t *e)
{
    (void)e;
    const char *why = NULL;
    if (hermes_bridge_control("new", &why)) {
        hermes_panels_close();
    } else if (why) {
        toast_warn(why);
    }
}

static void session_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const char *why = NULL;
    if (hermes_bridge_select_session(s_p.ids[i], &why)) {
        ESP_LOGI(TAG, "switching to session %s", s_p.ids[i]);
        hermes_panels_close();
    } else if (why) {
        toast_warn(why);
    }
}

static void open_jobs_cb(lv_event_t *e)
{
    (void)e;
    hermes_panels_open(s_p.root, s_p.top, HERMES_PANEL_JOBS);
}

static void open_setup_cb(lv_event_t *e)
{
    (void)e;
    hermes_panels_open(s_p.root, s_p.top, HERMES_PANEL_SETUP);
}

static void draw_drawer(const hermes_chat_t *c)
{
    lv_obj_t *actions = lv_obj_create(s_p.body);
    lv_obj_remove_style_all(actions);
    lv_obj_add_style(actions, (lv_style_t *)&hermes_view_styles()->row, LV_PART_MAIN);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW_WRAP);
    plain_button(actions, LV_SYMBOL_PLUS " New", new_conversation_cb, 0);
    plain_button(actions, "Scheduled", open_jobs_cb, 0);
    plain_button(actions, "Address", open_setup_cb, 0);

    note(s_p.body, c->registration[0] ? c->registration : "Conversations");
    s_p.drawn = c->sessions_revision;
    if (c->sessions_state != HERMES_LIST_LOADED) {
        note(s_p.body, list_sentence(c->sessions_state, "Asking Hermes...",
                                     "Hermes cannot list this device's conversations now."));
        return;
    }
    if (c->session_count == 0) {
        note(s_p.body, "No conversations yet.");
        return;
    }
    for (uint8_t i = 0; i < c->session_count && i < ROWS; i++) {
        const hermes_session_t *s = &c->sessions[i];
        snprintf(s_p.ids[i], sizeof(s_p.ids[i]), "%s", s->id);
        char age[16], sub[64];
        hermes_ui_age(s->age_seconds, age, sizeof(age));
        snprintf(sub, sizeof(sub), "%s%u messages, %s%s", s->current ? "This one. " : "",
                 (unsigned)s->messages, age, strcmp(age, "now") == 0 ? "" : " ago");
        row(s_p.body, s->label, sub, session_cb, i, s->current);
    }
}

/* ------------------------------------------------------------------ */
/* The Command panel                                                    */
/* ------------------------------------------------------------------ */

static void command_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char text[HERMES_CHAT_COMMAND_NAME_BYTES + 4];
    snprintf(text, sizeof(text), "/%s ", s_p.names[i]);
    hermes_panels_close();
    hermes_ui_insert(text);
}

static void draw_commands(const hermes_chat_t *c)
{
    s_p.drawn = c->commands_revision;
    if (c->command_count == 0) {
        note(s_p.body, hermes_ui_link()->link == HERMES_LINK_READY
                           ? "Hermes sent no commands."
                           : "The commands arrive with the connection to Hermes.");
        return;
    }
    /* Sixteen fit the pool comfortably; the rest are Hermes's to /help. */
    for (uint8_t i = 0; i < c->command_count && i < ROWS; i++) {
        const hermes_command_t *cmd = &c->commands[i];
        snprintf(s_p.names[i], sizeof(s_p.names[i]), "%s", cmd->name);
        char title[HERMES_CHAT_COMMAND_NAME_BYTES + HERMES_CHAT_COMMAND_ARGS_BYTES + 4];
        snprintf(title, sizeof(title), "/%s%s%s", cmd->name, cmd->args_hint[0] ? " " : "", cmd->args_hint);
        row(s_p.body, title, cmd->description, command_cb, i, false);
    }
    if (c->command_count > ROWS) {
        char more[64];
        snprintf(more, sizeof(more), "And %u more: type / and the name.",
                 (unsigned)(c->command_count - ROWS));
        note(s_p.body, more);
    }
}

/* ------------------------------------------------------------------ */
/* Scheduled Runs                                                       */
/* ------------------------------------------------------------------ */

static void ask_for_run(uint32_t offset)
{
    s_p.offset = offset;
    s_p.why = NULL;
    const char *why = NULL;
    if (!hermes_bridge_request_run(s_p.job_id, offset, &why)) s_p.why = why;
}

static void job_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    snprintf(s_p.job_id, sizeof(s_p.job_id), "%s", s_p.ids[i]);
    snprintf(s_p.job_name, sizeof(s_p.job_name), "%s", s_p.names[i]);
    hermes_panels_open(s_p.root, s_p.top, HERMES_PANEL_RUN);
}

static void draw_jobs(const hermes_chat_t *c)
{
    s_p.drawn = c->scheduled_revision;
    if (c->jobs_state != HERMES_LIST_LOADED) {
        note(s_p.body, list_sentence(c->jobs_state, "Asking Hermes...",
                                     "Hermes could not list its Scheduled Runs. Try again later."));
        return;
    }
    if (c->job_count == 0) {
        note(s_p.body, "No Scheduled Job has delivered anything yet.");
        return;
    }
    for (uint8_t i = 0; i < c->job_count && i < ROWS; i++) {
        const hermes_job_t *job = &c->jobs[i];
        snprintf(s_p.ids[i], sizeof(s_p.ids[i]), "%s", job->job_id);
        snprintf(s_p.names[i], sizeof(s_p.names[i]), "%s", job->name);
        char age[16], sub[HERMES_SCHEDULED_SCHEDULE_BYTES + 48];
        hermes_ui_age(job->age_seconds, age, sizeof(age));
        int n = snprintf(sub, sizeof(sub), "%s%slast %s%s", job->schedule, job->schedule[0] ? ", " : "",
                         age, strcmp(age, "now") == 0 ? "" : " ago");
        if (job->unread && n > 0 && (size_t)n < sizeof(sub)) {
            snprintf(sub + n, sizeof(sub) - (size_t)n, ", %u new", (unsigned)job->unread);
        }
        row(s_p.body, job->name, sub, job_cb, i, false);
    }
}

static void older_cb(lv_event_t *e)
{
    (void)e;
    ask_for_run(s_p.offset + 1);
    hermes_ui_refresh_chat();
    draw();
}

static void newer_cb(lv_event_t *e)
{
    (void)e;
    if (s_p.offset > 0) ask_for_run(s_p.offset - 1);
    hermes_ui_refresh_chat();
    draw();
}

static void draw_run(const hermes_chat_t *c)
{
    const hermes_run_t *run = &c->run;
    s_p.drawn = c->scheduled_revision;
    bool ours = strcmp(run->job_id, s_p.job_id) == 0 && run->offset == s_p.offset;
    if (!ours || run->state != HERMES_LIST_LOADED) {
        note(s_p.body, list_sentence(ours ? run->state : HERMES_LIST_NOT_ASKED, "Asking Hermes...",
                                     "Hermes could not read this run. Try again later."));
    } else if (run->absent) {
        note(s_p.body, "No older run is kept.");
    } else {
        char age[16], when[HERMES_SCHEDULED_CAPTURED_BYTES + 32];
        hermes_ui_age(run->age_seconds, age, sizeof(age));
        snprintf(when, sizeof(when), "%s%s%s%s", run->captured, run->captured[0] ? ", " : "", age,
                 strcmp(age, "now") == 0 ? "" : " ago");
        note(s_p.body, when);
        hermes_arena_free(&s_p.arena);
        if (hermes_arena_init(&s_p.arena, ARENA_BYTES)) {
            int32_t width = lv_obj_get_content_width(s_p.body);
            for (uint8_t i = 0; i < run->part_count; i++) {
                hermes_view_markdown(s_p.body, &s_p.arena, run->parts[i], width);
            }
        }
        if (run->truncated) note(s_p.body, "Hermes delivered more than the bridge kept.");
        if (run->parts_dropped) note(s_p.body, "Too long to show all of it here.");
    }

    lv_obj_t *nav = lv_obj_create(s_p.body);
    lv_obj_remove_style_all(nav);
    lv_obj_add_style(nav, (lv_style_t *)&hermes_view_styles()->row, LV_PART_MAIN);
    lv_obj_set_size(nav, LV_PCT(100), LV_SIZE_CONTENT);
    if (ours && run->state == HERMES_LIST_LOADED && !run->absent && run->older) {
        plain_button(nav, LV_SYMBOL_LEFT " Older", older_cb, 0);
    }
    if (s_p.offset > 0) plain_button(nav, "Newer " LV_SYMBOL_RIGHT, newer_cb, 0);
}

/* ------------------------------------------------------------------ */
/* The bridge's address                                                 */
/* ------------------------------------------------------------------ */

static void connect_cb(lv_event_t *e)
{
    (void)e;
    hermes_ws_url_t url;
    const char *error = hermes_ws_parse_url(lv_textarea_get_text(s_p.url), &url);
    if (error) {
        lv_label_set_text(s_p.url_error, error);
        lv_obj_remove_flag(s_p.url_error, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char text[HERMES_WS_URL_MAX];
    hermes_ws_format_url(&url, text, sizeof(text));
    if (!hermes_config_save(text)) toast_warn("No SD card to keep it on: used until the app closes.");
    hermes_ui_set_url(text);
    hermes_panels_close();
}

static void url_key_cb(lv_event_t *e)
{
    if (lv_event_get_key(e) == LV_KEY_ENTER) connect_cb(e);
}

static void peer_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char text[HERMES_WS_URL_MAX];
    snprintf(text, sizeof(text), "ws://%s:%u", s_p.peers[i].ip, (unsigned)HERMES_WS_DEFAULT_PORT);
    lv_textarea_set_text(s_p.url, text);
    lv_group_t *group = lv_group_get_default();
    if (group) lv_group_focus_obj(s_p.url);
}

static void draw_setup(void)
{
    note(s_p.body, "Your Hermes bridge, on the tailnet:");
    s_p.url = lv_textarea_create(s_p.body);
    lv_textarea_set_one_line(s_p.url, true);
    lv_textarea_set_max_length(s_p.url, HERMES_WS_URL_MAX - 1);
    lv_textarea_set_placeholder_text(s_p.url, "ws://homelab:8765");
    lv_obj_set_width(s_p.url, LV_PCT(100));
    lv_obj_add_style(s_p.url, (lv_style_t *)&hermes_view_styles()->field, LV_PART_MAIN);
    const char *now = hermes_ui_url();
    lv_textarea_set_text(s_p.url, now[0] ? now : hermes_config_built_in());
    lv_obj_add_event_cb(s_p.url, url_key_cb, LV_EVENT_KEY, NULL);
    focusable(s_p.url);

    s_p.url_error = note(s_p.body, "");
    lv_obj_add_flag(s_p.url_error, LV_OBJ_FLAG_HIDDEN);
    plain_button(s_p.body, "Connect", connect_cb, 0);

    if (!tailnet_is_connected()) {
        note(s_p.body, "The tailnet's machines are listed once Tailscale is connected.");
        return;
    }
    int count = tailnet_peer_count();
    if (count <= 0) return;
    note(s_p.body, "On your tailnet:");
    for (int i = 0, shown = 0; i < count && shown < PEERS; i++) {
        tailnet_peer_t *peer = &s_p.peers[shown];
        if (tailnet_get_peer(i, peer) != ESP_OK) continue;
        char name[sizeof(peer->name)];
        snprintf(name, sizeof(name), "%s", peer->name);
        char *dot = strchr(name, '.');
        if (dot) *dot = '\0';
        row(s_p.body, name, peer->ip, peer_cb, shown, false);
        shown++;
    }
}

/* ------------------------------------------------------------------ */
/* Opening and closing                                                  */
/* ------------------------------------------------------------------ */

static const char *title_of(hermes_panel_t kind)
{
    switch (kind) {
    case HERMES_PANEL_DRAWER: return "Hermes";
    case HERMES_PANEL_COMMANDS: return "Commands";
    case HERMES_PANEL_JOBS: return "Scheduled Runs";
    case HERMES_PANEL_RUN: return s_p.job_name[0] ? s_p.job_name : "Scheduled Run";
    case HERMES_PANEL_SETUP: return "Bridge address";
    default: return "";
    }
}

static void draw(void)
{
    if (!s_p.panel) return;
    lv_obj_clean(s_p.body);
    s_p.first = NULL;
    s_p.url = s_p.url_error = NULL;
    const hermes_chat_t *c = hermes_ui_chat();
    switch (s_p.kind) {
    case HERMES_PANEL_DRAWER: draw_drawer(c); break;
    case HERMES_PANEL_COMMANDS: draw_commands(c); break;
    case HERMES_PANEL_JOBS: draw_jobs(c); break;
    case HERMES_PANEL_RUN: draw_run(c); break;
    case HERMES_PANEL_SETUP: draw_setup(); break;
    default: break;
    }
    lv_group_t *group = lv_group_get_default();
    if (group && s_p.first) lv_group_focus_obj(s_p.first);
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    hermes_panels_back();
}

static void build_frame(void)
{
    const hermes_view_styles_t *st = hermes_view_styles();
    s_p.panel = lv_obj_create(s_p.root);
    lv_obj_remove_style_all(s_p.panel);
    lv_obj_add_style(s_p.panel, (lv_style_t *)&st->page, LV_PART_MAIN);
    lv_obj_add_flag(s_p.panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(s_p.panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(s_p.root);
    lv_obj_set_pos(s_p.panel, 0, s_p.top);
    lv_obj_set_size(s_p.panel, lv_obj_get_width(s_p.root), lv_obj_get_height(s_p.root) - s_p.top);
    lv_obj_set_flex_flow(s_p.panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_p.panel, PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_p.panel, PAD, LV_PART_MAIN);

    lv_obj_t *top = lv_obj_create(s_p.panel);
    lv_obj_remove_style_all(top);
    lv_obj_add_style(top, (lv_style_t *)&st->row, LV_PART_MAIN);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_p.title = lv_label_create(top);
    lv_obj_add_style(s_p.title, (lv_style_t *)&st->heading, LV_PART_MAIN);
    lv_label_set_long_mode(s_p.title, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(s_p.title, 1);
    lv_label_set_text(s_p.title, "");

    lv_obj_t *back = lv_button_create(top);
    lv_obj_remove_style_all(back);
    lv_obj_add_style(back, (lv_style_t *)&st->button, LV_PART_MAIN);
    lv_obj_add_style(back, (lv_style_t *)&st->button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(back, (lv_style_t *)&st->focus, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_size(back, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(back), LV_SYMBOL_CLOSE);
    hermes_ui_add_focus(back);

    s_p.body = lv_obj_create(s_p.panel);
    lv_obj_remove_style_all(s_p.body);
    lv_obj_add_style(s_p.body, (lv_style_t *)&st->box, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_p.body, PAD, LV_PART_MAIN);
    lv_obj_set_width(s_p.body, LV_PCT(100));
    lv_obj_set_flex_grow(s_p.body, 1);
    lv_obj_set_scroll_dir(s_p.body, LV_DIR_VER);
}

void hermes_panels_open(lv_obj_t *root, int32_t top, hermes_panel_t kind)
{
    if (!root) return;
    s_p.root = root;
    s_p.top = top;
    if (!s_p.panel) build_frame();
    s_p.kind = kind;
    s_p.why = NULL;
    s_p.drawn = 0;
    lv_label_set_text(s_p.title, title_of(kind));

    const char *why = NULL;
    if (kind == HERMES_PANEL_DRAWER && !hermes_bridge_request_sessions(&why)) s_p.why = why;
    if (kind == HERMES_PANEL_JOBS && !hermes_bridge_request_jobs(&why)) s_p.why = why;
    if (kind == HERMES_PANEL_RUN) ask_for_run(0);
    /* What was just asked for is waiting now, not what it was a tick ago. */
    hermes_ui_refresh_chat();
    ESP_LOGI(TAG, "panel: %s", title_of(kind));
    draw();
}

void hermes_panels_close(void)
{
    if (s_p.panel) lv_obj_delete(s_p.panel);
    /* The run's labels pointed into this. */
    hermes_arena_free(&s_p.arena);
    bool was_open = s_p.panel != NULL;
    s_p.panel = s_p.body = s_p.title = s_p.first = s_p.url = s_p.url_error = NULL;
    s_p.kind = HERMES_PANEL_NONE;
    if (was_open) hermes_ui_panel_closed();
}

hermes_panel_t hermes_panels_current(void)
{
    return s_p.panel ? s_p.kind : HERMES_PANEL_NONE;
}

bool hermes_panels_back(void)
{
    switch (hermes_panels_current()) {
    case HERMES_PANEL_NONE:
        return false;
    case HERMES_PANEL_RUN:
        hermes_panels_open(s_p.root, s_p.top, HERMES_PANEL_JOBS);
        return true;
    case HERMES_PANEL_JOBS:
        hermes_panels_open(s_p.root, s_p.top, HERMES_PANEL_DRAWER);
        return true;
    default:
        hermes_panels_close();
        return true;
    }
}

void hermes_panels_refresh(void)
{
    if (!s_p.panel) return;
    const hermes_chat_t *c = hermes_ui_chat();
    uint32_t now;
    switch (s_p.kind) {
    case HERMES_PANEL_DRAWER: now = c->sessions_revision; break;
    case HERMES_PANEL_COMMANDS: now = c->commands_revision; break;
    case HERMES_PANEL_JOBS:
    case HERMES_PANEL_RUN: now = c->scheduled_revision; break;
    default: return;
    }
    if (now != s_p.drawn) draw();
}
