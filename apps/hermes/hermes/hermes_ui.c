/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Hermes app's screen.
 *
 *   ┌──────────────────────────────┐
 *   │ Hermes        online  [/][≡] │  header: the link, the Command panel,
 *   │                              │  the drawer
 *   │ ┌──────────────────────────┐ │
 *   │ │ What will it cost today? │ │  your message, boxed
 *   │ └──────────────────────────┘ │
 *   │ ⟳ Reading prices             │  a tool Hermes is running
 *   │ Prices today                 │  Hermes's reply, as Markdown
 *   │ Hour   Price                 │
 *   │ 06:00  6.20 c/kWh            │
 *   │ [Message Hermes        ] [>] │  Enter sends; [■] stops a turn
 *   └──────────────────────────────┘
 *
 * Until the first Bridge Session is up, a page says what it waits for:
 * Tailscale switched on, signed in, connected; the bridge's address; the
 * bridge answering, or why it does not. After that the transcript stays on
 * screen whatever happens to the link, and the header says how it is.
 *
 * A timer reads the tailnet, the link and a copy of the chat state five
 * times a second, and changes only what changed: the e-paper redraws
 * whatever is set. The transcript is redrawn from the first item that
 * differs, so a reply arriving draws the reply, not the conversation.
 */
#include "hermes/hermes_ui.h"
#include "hermes/hermes_config.h"
#include "hermes/hermes_view.h"

#include "thistle/app_manager.h"
#include "thistle/tailnet.h"
#include "ui/manager.h"
#include "ui/toast.h"

#include "esp_attr.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "hermes_ui";

#define TICK_MS      200
#define HEADER_H     30
#define BAR_H        38
#define PAD          6
#define ARENA_BYTES  4096
#define ITEMS        (HERMES_CHAT_ROWS + HERMES_CHAT_ACTIVITIES + 2)
#define DRAFT_MAX    2000

typedef enum {
    ITEM_MESSAGE,
    ITEM_ACTIVITY,
    ITEM_HISTORY,   /* Hermes could not supply the history */
    ITEM_HINT,      /* an empty conversation */
} item_kind_t;

typedef struct {
    item_kind_t kind;
    char id[HERMES_CHAT_ID_BYTES];
    uint32_t stamp;
    lv_obj_t *box;
    hermes_arena_t arena;
} item_t;

typedef enum {
    ACT_NONE,
    ACT_TAILNET_ON,
    ACT_TAILSCALE_APP,
    ACT_RETRY,
    ACT_SETUP,
} action_t;

/* Only the LVGL task touches this. In PSRAM: the chat state's copy alone is
 * tens of kilobytes. */
static EXT_RAM_BSS_ATTR struct {
    lv_obj_t *root;
    lv_obj_t *status;
    lv_obj_t *commands_btn;
    lv_obj_t *menu_btn;
    lv_obj_t *page;
    lv_obj_t *page_heading;
    lv_obj_t *page_text;
    lv_obj_t *page_buttons[2];
    lv_obj_t *page_labels[2];
    action_t actions[2];
    lv_obj_t *list;
    lv_obj_t *bar;
    lv_obj_t *input;
    lv_obj_t *send_btn;
    lv_obj_t *send_label;
    lv_timer_t *tick;

    hermes_chat_t chat;
    uint32_t chat_revision;
    hermes_link_status_t link;
    tailnet_status_t tailnet;
    char url[HERMES_WS_URL_MAX];
    bool have_url;
    bool setup_offered;
    bool chat_shown;
    bool follow;
    bool stop_shown;
    uint32_t drawn_transcript;
    item_t items[ITEMS];
    uint8_t item_count;
} s_ui;

/* ------------------------------------------------------------------ */
/* Small things                                                         */
/* ------------------------------------------------------------------ */

static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void set_shown(lv_obj_t *obj, bool shown)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == !shown) return;
    if (shown) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

void hermes_ui_add_focus(lv_obj_t *obj)
{
    lv_group_t *group = lv_group_get_default();
    if (group && obj) lv_group_add_obj(group, obj);
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *data,
                        lv_obj_t **label_out)
{
    const hermes_view_styles_t *st = hermes_view_styles();
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, (lv_style_t *)&st->button, LV_PART_MAIN);
    lv_obj_add_style(btn, (lv_style_t *)&st->button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(btn, (lv_style_t *)&st->focus, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, data);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (label_out) *label_out = label;
    hermes_ui_add_focus(btn);
    return btn;
}

void hermes_ui_age(uint32_t seconds, char *out, size_t cap)
{
    if (seconds < 60) snprintf(out, cap, "now");
    else if (seconds < 3600) snprintf(out, cap, "%u min", (unsigned)(seconds / 60));
    else if (seconds < 86400) snprintf(out, cap, "%u h", (unsigned)(seconds / 3600));
    else snprintf(out, cap, "%u d", (unsigned)(seconds / 86400));
}

const hermes_chat_t *hermes_ui_chat(void)
{
    return &s_ui.chat;
}

void hermes_ui_refresh_chat(void)
{
    hermes_bridge_snapshot(&s_ui.chat, &s_ui.chat_revision);
}

const hermes_link_status_t *hermes_ui_link(void)
{
    return &s_ui.link;
}

const char *hermes_ui_url(void)
{
    return s_ui.have_url ? s_ui.url : "";
}

void hermes_ui_set_url(const char *url)
{
    snprintf(s_ui.url, sizeof(s_ui.url), "%s", url);
    s_ui.have_url = url[0] != '\0';
    ESP_LOGI(TAG, "bridge address %s", s_ui.have_url ? s_ui.url : "cleared");
    /* A session to the old address goes at the next tick. */
    hermes_bridge_stop();
}

void hermes_ui_insert(const char *text)
{
    if (!s_ui.input) return;
    lv_textarea_add_text(s_ui.input, text);
}

static void focus_input(void)
{
    lv_group_t *group = lv_group_get_default();
    if (group && s_ui.input && s_ui.chat_shown) lv_group_focus_obj(s_ui.input);
}

void hermes_ui_panel_closed(void)
{
    focus_input();
}

/* ------------------------------------------------------------------ */
/* The transcript                                                       */
/* ------------------------------------------------------------------ */

static void free_items_from(uint8_t first)
{
    for (uint8_t i = first; i < s_ui.item_count; i++) {
        if (s_ui.items[i].box) lv_obj_delete(s_ui.items[i].box);
        hermes_arena_free(&s_ui.items[i].arena);
        memset(&s_ui.items[i], 0, sizeof(s_ui.items[i]));
    }
    if (first < s_ui.item_count) s_ui.item_count = first;
}

static void want(item_t *list, uint8_t *count, item_kind_t kind, const char *id, uint32_t stamp)
{
    if (*count >= ITEMS) return;
    item_t *item = &list[(*count)++];
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    snprintf(item->id, sizeof(item->id), "%s", id);
    item->stamp = stamp;
}

/* What a message is drawn from, as a number: a reconnect's sync sends the
 * same messages again, and they need no drawing again. FNV-1a. */
static uint32_t message_stamp(const hermes_row_t *row)
{
    uint32_t h = 2166136261u ^ (uint32_t)row->role;
    for (const unsigned char *p = (const unsigned char *)row->text; *p; p++) {
        h = (h ^ *p) * 16777619u;
    }
    return h;
}

/* What the transcript should show, in order: each activity row after the
 * message it was anchored at, so it draws inside the turn that made it. */
static uint8_t wanted_items(item_t *list)
{
    const hermes_chat_t *c = &s_ui.chat;
    uint8_t n = 0;
    bool done[HERMES_CHAT_ACTIVITIES] = { false };

    if (c->history_unavailable) want(list, &n, ITEM_HISTORY, "history", 0);
    for (uint8_t r = 0; r < c->row_count; r++) {
        const hermes_row_t *row = &c->rows[r];
        if (row->position > 0) {
            for (uint8_t a = 0; a < c->activity_count; a++) {
                if (done[a] || c->activities[a].anchor >= row->position) continue;
                done[a] = true;
                hermes_activity_shown_t shown = hermes_chat_activity_shown(c, a);
                if (shown != HERMES_ACTIVITY_HIDDEN) {
                    want(list, &n, ITEM_ACTIVITY, c->activities[a].id,
                         c->activities[a].arrival * 4 + shown);
                }
            }
        }
        want(list, &n, ITEM_MESSAGE, row->id, message_stamp(row));
    }
    for (uint8_t a = 0; a < c->activity_count; a++) {
        if (done[a]) continue;
        hermes_activity_shown_t shown = hermes_chat_activity_shown(c, a);
        if (shown != HERMES_ACTIVITY_HIDDEN) {
            want(list, &n, ITEM_ACTIVITY, c->activities[a].id, c->activities[a].arrival * 4 + shown);
        }
    }
    if (c->row_count == 0 && n == (c->history_unavailable ? 1 : 0)) {
        want(list, &n, ITEM_HINT, "hint", 0);
    }
    return n;
}

static const hermes_row_t *find_row(const char *id)
{
    for (uint8_t r = 0; r < s_ui.chat.row_count; r++) {
        if (strcmp(s_ui.chat.rows[r].id, id) == 0) return &s_ui.chat.rows[r];
    }
    return NULL;
}

static uint8_t find_activity(const char *id)
{
    for (uint8_t a = 0; a < s_ui.chat.activity_count; a++) {
        if (strcmp(s_ui.chat.activities[a].id, id) == 0) return a;
    }
    return 0xFF;
}

static void draw_item(item_t *item)
{
    const hermes_view_styles_t *st = hermes_view_styles();
    if (!hermes_arena_init(&item->arena, ARENA_BYTES)) return;
    lv_obj_t *box = lv_obj_create(s_ui.list);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, (lv_style_t *)&st->box, LV_PART_MAIN);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    item->box = box;
    int32_t width = lv_obj_get_content_width(s_ui.list);
    char line[HERMES_CHAT_ACTIVITY_LABEL_BYTES + HERMES_CHAT_ACTIVITY_DETAIL_BYTES + 16];

    switch (item->kind) {
    case ITEM_HISTORY:
        snprintf(line, sizeof(line), "Hermes could not supply this conversation's history.");
        hermes_view_label(box, &item->arena, line, strlen(line), &st->secondary);
        break;
    case ITEM_HINT:
        snprintf(line, sizeof(line), "Ask Hermes anything. Hold the mic key to dictate.");
        hermes_view_label(box, &item->arena, line, strlen(line), &st->secondary);
        break;
    case ITEM_ACTIVITY: {
        uint8_t a = find_activity(item->id);
        if (a == 0xFF) break;
        const hermes_activity_t *act = &s_ui.chat.activities[a];
        const char *mark = act->state == HERMES_ACTIVITY_RUNNING ? LV_SYMBOL_REFRESH
                         : act->state == HERMES_ACTIVITY_FAILED ? LV_SYMBOL_CLOSE : LV_SYMBOL_OK;
        bool expanded = hermes_chat_activity_shown(&s_ui.chat, a) == HERMES_ACTIVITY_EXPANDED;
        snprintf(line, sizeof(line), "%s %s%s%s", mark, act->label,
                 expanded && act->detail[0] ? "\n" : "", expanded ? act->detail : "");
        hermes_view_label(box, &item->arena, line, strlen(line), &st->secondary);
        break;
    }
    case ITEM_MESSAGE: {
        const hermes_row_t *row = find_row(item->id);
        if (!row) break;
        if (row->role == HERMES_ROLE_WARNING || row->role == HERMES_ROLE_ERROR) {
            static EXT_RAM_BSS_ATTR char note[HERMES_CHAT_TEXT_BYTES + 8];
            snprintf(note, sizeof(note), "%s %s", LV_SYMBOL_WARNING, row->text);
            hermes_view_label(box, &item->arena, note, strlen(note), &st->secondary);
            break;
        }
        if (row->role == HERMES_ROLE_USER) {
            lv_obj_add_style(box, (lv_style_t *)&st->user, LV_PART_MAIN);
            width -= 2 * (4 + 1);
        }
        hermes_view_markdown(box, &item->arena, row->text, width);
        break;
    }
    }
}

static void update_transcript(void)
{
    if (!s_ui.list) return;
    /* A session that has not synced, or is syncing, has nothing worth
     * replacing what is on screen with. */
    if (!s_ui.chat.conversation_id[0] || s_ui.chat.syncing) return;
    if (s_ui.chat.transcript_revision == s_ui.drawn_transcript && s_ui.item_count) return;
    s_ui.drawn_transcript = s_ui.chat.transcript_revision;

    static EXT_RAM_BSS_ATTR item_t wanted[ITEMS];
    uint8_t n = wanted_items(wanted);
    uint8_t same = 0;
    while (same < n && same < s_ui.item_count && wanted[same].kind == s_ui.items[same].kind &&
           wanted[same].stamp == s_ui.items[same].stamp &&
           strcmp(wanted[same].id, s_ui.items[same].id) == 0) {
        same++;
    }
    if (same == n && same == s_ui.item_count) return;

    free_items_from(same);
    /* Laid out before it is measured: it was hidden until the chat was
     * shown, and a table is drawn as a grid only if it fits the width. */
    lv_obj_update_layout(s_ui.list);
    for (uint8_t i = same; i < n; i++) {
        s_ui.items[i] = wanted[i];
        s_ui.item_count = (uint8_t)(i + 1);
        draw_item(&s_ui.items[i]);
    }
    ESP_LOGI(TAG, "transcript: %u items, %u redrawn", n, (unsigned)(n - same));
    if (s_ui.follow) {
        lv_obj_update_layout(s_ui.list);
        lv_obj_scroll_to_y(s_ui.list, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static void list_scrolled_cb(lv_event_t *e)
{
    (void)e;
    /* Following the newest content until the reader scrolls back. */
    s_ui.follow = lv_obj_get_scroll_bottom(s_ui.list) <= 8;
}

/* ------------------------------------------------------------------ */
/* Sending                                                              */
/* ------------------------------------------------------------------ */

static void send_draft(void)
{
    const char *text = lv_textarea_get_text(s_ui.input);
    const char *why = NULL;
    if (!text || !text[0]) return;
    if (hermes_bridge_submit(text, &why)) {
        lv_textarea_set_text(s_ui.input, "");
        s_ui.follow = true;
    } else if (why) {
        toast_warn(why);
    }
}

static void send_cb(lv_event_t *e)
{
    (void)e;
    const char *why = NULL;
    if (s_ui.stop_shown) {
        if (!hermes_bridge_control("stop", &why) && why) toast_warn(why);
        return;
    }
    send_draft();
}

static void input_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ENTER) send_draft();
    else if (key == LV_KEY_ESC) app_manager_launch("com.thistle.launcher");
}

static void commands_cb(lv_event_t *e)
{
    (void)e;
    hermes_panels_open(s_ui.root, HEADER_H, HERMES_PANEL_COMMANDS);
}

static void menu_cb(lv_event_t *e)
{
    (void)e;
    hermes_panels_open(s_ui.root, HEADER_H, HERMES_PANEL_DRAWER);
}

/* ------------------------------------------------------------------ */
/* The waiting page                                                     */
/* ------------------------------------------------------------------ */

static void page_button_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    switch (s_ui.actions[i]) {
    case ACT_TAILNET_ON:
        tailnet_set_enabled(true);
        break;
    case ACT_TAILSCALE_APP:
        app_manager_launch("com.thistle.tailscale");
        break;
    case ACT_RETRY:
        hermes_bridge_retry_now();
        break;
    case ACT_SETUP:
        hermes_panels_open(s_ui.root, HEADER_H, HERMES_PANEL_SETUP);
        break;
    default:
        break;
    }
}

static void set_page_button(int i, action_t action, const char *text)
{
    s_ui.actions[i] = action;
    set_shown(s_ui.page_buttons[i], action != ACT_NONE);
    if (action != ACT_NONE) set_text(s_ui.page_labels[i], text);
}

static void show_page(const char *heading, const char *text, action_t a0, const char *t0,
                      action_t a1, const char *t1)
{
    set_text(s_ui.page_heading, heading);
    set_text(s_ui.page_text, text);
    set_page_button(0, a0, t0);
    set_page_button(1, a1, t1);
}

static void update_page(void)
{
    const tailnet_status_t *tn = &s_ui.tailnet;
    const hermes_link_status_t *link = &s_ui.link;
    char text[384];

    switch (tn->state) {
    case TAILNET_UNAVAILABLE:
        show_page("Hermes needs Tailscale",
                  "This board has no Tailscale client, so it cannot reach the Hermes bridge.",
                  ACT_NONE, NULL, ACT_NONE, NULL);
        return;
    case TAILNET_OFF:
        show_page("Tailscale is off",
                  "Hermes is reached over your tailnet. Switch Tailscale on to connect.",
                  ACT_TAILNET_ON, "Switch on", ACT_TAILSCALE_APP, "Tailscale");
        return;
    case TAILNET_NEEDS_LOGIN:
        show_page("Sign in to Tailscale",
                  "This device has not joined your tailnet yet. Sign in from the Tailscale app.",
                  ACT_TAILSCALE_APP, "Tailscale", ACT_NONE, NULL);
        return;
    case TAILNET_NEEDS_APPROVAL:
        show_page("Waiting for approval",
                  "An admin of your tailnet must approve this device first.",
                  ACT_TAILSCALE_APP, "Tailscale", ACT_NONE, NULL);
        return;
    case TAILNET_NO_NETWORK:
        show_page("Waiting for Tailscale", "Tailscale is waiting for WiFi or mobile data.",
                  ACT_NONE, NULL, ACT_NONE, NULL);
        return;
    case TAILNET_NO_CLOCK:
        show_page("Waiting for Tailscale", "Tailscale is waiting for the clock to be set.",
                  ACT_NONE, NULL, ACT_NONE, NULL);
        return;
    case TAILNET_CONNECTING:
        show_page("Waiting for Tailscale", "Joining your tailnet...", ACT_NONE, NULL, ACT_NONE, NULL);
        return;
    case TAILNET_FAILED:
        snprintf(text, sizeof(text), "Tailscale stopped: %s", tn->error[0] ? tn->error : "no reason given.");
        show_page("Tailscale stopped", text, ACT_TAILSCALE_APP, "Tailscale", ACT_NONE, NULL);
        return;
    case TAILNET_CONNECTED:
        break;
    }

    if (!s_ui.have_url) {
        show_page("Where is Hermes?",
                  "Give the address of your Hermes bridge on the tailnet, such as ws://homelab:8765.",
                  ACT_SETUP, "Set address", ACT_NONE, NULL);
        return;
    }

    switch (link->link) {
    case HERMES_LINK_REFUSED:
        snprintf(text, sizeof(text),
                 "The bridge admits devices tagged tag:hermes-device. In the Tailscale admin "
                 "console, give %s that tag, and let the tag reach %s. Tried again every 30 s.",
                 tn->hostname[0] ? tn->hostname : "this device", link->url);
        show_page("Hermes refused this device", text, ACT_RETRY, "Try now", ACT_SETUP, "Address");
        break;
    case HERMES_LINK_INCOMPATIBLE:
        snprintf(text, sizeof(text), "%s",
                 strcmp(link->update, "plugin") == 0
                     ? "The bridge speaks an older protocol than this app. Update the Hermes "
                       "plugin on the bridge's host."
                     : "This app speaks an older protocol than the bridge. Update ThistleOS.");
        show_page("An update is needed", text, ACT_RETRY, "Try again", ACT_NONE, NULL);
        break;
    case HERMES_LINK_WAITING:
        /* No countdown: on e-paper it would redraw the page every second. */
        snprintf(text, sizeof(text), "%s%sTrying again shortly.",
                 link->reason[0] ? link->reason : "", link->reason[0] ? " " : "");
        show_page("Cannot reach Hermes", text, ACT_RETRY, "Try now", ACT_SETUP, "Address");
        break;
    default:
        snprintf(text, sizeof(text), "%s%s%s", s_ui.url, link->reason[0] ? "\n" : "", link->reason);
        show_page(link->link == HERMES_LINK_SYNCING ? "Loading the conversation" : "Connecting to Hermes",
                  text, ACT_NONE, NULL, ACT_SETUP, "Address");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* The header                                                           */
/* ------------------------------------------------------------------ */

static const char *status_word(void)
{
    if (s_ui.tailnet.state != TAILNET_CONNECTED) {
        return s_ui.tailnet.state == TAILNET_OFF ? "no tailnet" : "tailnet...";
    }
    if (!s_ui.have_url) return "";
    switch (s_ui.link.link) {
    case HERMES_LINK_READY:
        return hermes_chat_busy(&s_ui.chat) ? "thinking" : "online";
    case HERMES_LINK_SYNCING:
        return "loading";
    case HERMES_LINK_REFUSED:
        return "refused";
    case HERMES_LINK_INCOMPATIBLE:
        return "update";
    case HERMES_LINK_WAITING:
        return "offline";
    default:
        return "connecting";
    }
}

static void update_bar(void)
{
    bool stop = s_ui.chat_shown && s_ui.link.link == HERMES_LINK_READY && hermes_chat_busy(&s_ui.chat);
    if (stop == s_ui.stop_shown) return;
    s_ui.stop_shown = stop;
    set_text(s_ui.send_label, stop ? LV_SYMBOL_STOP : LV_SYMBOL_RIGHT);
}

/* ------------------------------------------------------------------ */
/* The tick                                                             */
/* ------------------------------------------------------------------ */

static void show_chat(void)
{
    s_ui.chat_shown = true;
    set_shown(s_ui.page, false);
    set_shown(s_ui.list, true);
    set_shown(s_ui.bar, true);
    if (hermes_panels_current() == HERMES_PANEL_NONE) focus_input();
    ESP_LOGI(TAG, "chat shown");
}

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    tailnet_get_status(&s_ui.tailnet);
    bool tailnet_up = s_ui.tailnet.state == TAILNET_CONNECTED;
    if (tailnet_up && s_ui.have_url) hermes_bridge_start(s_ui.url);
    else hermes_bridge_stop();

    hermes_bridge_status(&s_ui.link);
    hermes_bridge_snapshot(&s_ui.chat, &s_ui.chat_revision);

    if (!s_ui.chat_shown && s_ui.link.link == HERMES_LINK_READY) show_chat();
    if (!s_ui.chat_shown) update_page();
    /* The first time the device is on the tailnet with nowhere to go. */
    if (tailnet_up && !s_ui.have_url && !s_ui.setup_offered &&
        hermes_panels_current() == HERMES_PANEL_NONE) {
        s_ui.setup_offered = true;
        hermes_panels_open(s_ui.root, HEADER_H, HERMES_PANEL_SETUP);
    }

    set_text(s_ui.status, status_word());
    /* Each compares its own revision, so a copy taken between ticks is
     * drawn too. */
    if (s_ui.chat_shown) update_transcript();
    hermes_panels_refresh();
    update_bar();
}

/* ------------------------------------------------------------------ */
/* Back                                                                 */
/* ------------------------------------------------------------------ */

static bool hermes_back(void)
{
    return hermes_panels_back();
}

/* ------------------------------------------------------------------ */
/* Building                                                             */
/* ------------------------------------------------------------------ */

esp_err_t hermes_ui_create(lv_obj_t *parent)
{
    if (!parent) parent = lv_screen_active();
    memset(&s_ui, 0, sizeof(s_ui));
    hermes_chat_init(&s_ui.chat);
    s_ui.follow = true;
    s_ui.have_url = hermes_config_url(s_ui.url, sizeof(s_ui.url));
    hermes_view_styles_init();
    const hermes_view_styles_t *st = hermes_view_styles();

    s_ui.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.root);
    lv_obj_add_style(s_ui.root, (lv_style_t *)&st->page, LV_PART_MAIN);
    lv_obj_set_size(s_ui.root, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_ui.root, LV_FLEX_FLOW_COLUMN);

    /* Header */
    lv_obj_t *header = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(header);
    lv_obj_add_style(header, (lv_style_t *)&st->bar, LV_PART_MAIN);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_size(header, LV_PCT(100), HEADER_H);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Hermes");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_flex_grow(title, 1);

    s_ui.status = lv_label_create(header);
    lv_obj_add_style(s_ui.status, (lv_style_t *)&st->secondary, LV_PART_MAIN);
    lv_label_set_text(s_ui.status, "");

    s_ui.commands_btn = button(header, "/", commands_cb, NULL, NULL);
    s_ui.menu_btn = button(header, LV_SYMBOL_BARS, menu_cb, NULL, NULL);
    lv_obj_set_style_pad_ver(s_ui.commands_btn, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_ui.menu_btn, 3, LV_PART_MAIN);

    /* The waiting page */
    s_ui.page = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(s_ui.page);
    lv_obj_add_style(s_ui.page, (lv_style_t *)&st->box, LV_PART_MAIN);
    lv_obj_set_width(s_ui.page, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.page, 1);
    lv_obj_set_style_pad_all(s_ui.page, PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_ui.page, PAD, LV_PART_MAIN);

    s_ui.page_heading = lv_label_create(s_ui.page);
    lv_obj_set_style_text_font(s_ui.page_heading, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.page_heading, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ui.page_heading, LV_PCT(100));
    lv_label_set_text(s_ui.page_heading, "");
    s_ui.page_text = lv_label_create(s_ui.page);
    lv_label_set_long_mode(s_ui.page_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ui.page_text, LV_PCT(100));
    lv_label_set_text(s_ui.page_text, "");

    lv_obj_t *buttons = lv_obj_create(s_ui.page);
    lv_obj_remove_style_all(buttons);
    lv_obj_add_style(buttons, (lv_style_t *)&st->row, LV_PART_MAIN);
    lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
    for (int i = 0; i < 2; i++) {
        s_ui.page_buttons[i] = button(buttons, "", page_button_cb, (void *)(intptr_t)i,
                                      &s_ui.page_labels[i]);
        lv_obj_add_flag(s_ui.page_buttons[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* The transcript */
    s_ui.list = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(s_ui.list);
    lv_obj_add_style(s_ui.list, (lv_style_t *)&st->box, LV_PART_MAIN);
    lv_obj_set_width(s_ui.list, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.list, 1);
    lv_obj_set_style_pad_all(s_ui.list, PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_ui.list, PAD, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ui.list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(s_ui.list, list_scrolled_cb, LV_EVENT_SCROLL_END, NULL);
    lv_obj_add_flag(s_ui.list, LV_OBJ_FLAG_HIDDEN);

    /* The compose bar */
    s_ui.bar = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(s_ui.bar);
    lv_obj_add_style(s_ui.bar, (lv_style_t *)&st->bar, LV_PART_MAIN);
    lv_obj_set_style_border_side(s_ui.bar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_size(s_ui.bar, LV_PCT(100), BAR_H);
    lv_obj_remove_flag(s_ui.bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_ui.bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_ui.bar, LV_OBJ_FLAG_HIDDEN);

    s_ui.input = lv_textarea_create(s_ui.bar);
    lv_textarea_set_one_line(s_ui.input, true);
    lv_textarea_set_max_length(s_ui.input, DRAFT_MAX);
    lv_textarea_set_placeholder_text(s_ui.input, "Message Hermes");
    lv_obj_set_flex_grow(s_ui.input, 1);
    lv_obj_add_style(s_ui.input, (lv_style_t *)&st->field, LV_PART_MAIN);
    lv_obj_set_height(s_ui.input, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s_ui.input, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_add_event_cb(s_ui.input, input_key_cb, LV_EVENT_KEY, NULL);
    hermes_ui_add_focus(s_ui.input);

    s_ui.send_btn = button(s_ui.bar, LV_SYMBOL_RIGHT, send_cb, NULL, &s_ui.send_label);
    lv_obj_set_style_pad_ver(s_ui.send_btn, 5, LV_PART_MAIN);

    ui_manager_set_back_handler(s_ui.root, hermes_back);
    s_ui.tick = lv_timer_create(tick_cb, TICK_MS, NULL);
    tick_cb(NULL);
    ESP_LOGI(TAG, "created; bridge %s", s_ui.have_url ? s_ui.url : "not set");
    return ESP_OK;
}

void hermes_ui_destroy(void)
{
    if (s_ui.tick) lv_timer_delete(s_ui.tick);
    s_ui.tick = NULL;
    hermes_bridge_stop();
    hermes_panels_close();
    if (s_ui.root) lv_obj_delete(s_ui.root);
    s_ui.root = NULL;
    /* The labels pointed into these: free them after the objects. */
    for (uint8_t i = 0; i < s_ui.item_count; i++) hermes_arena_free(&s_ui.items[i].arena);
    s_ui.item_count = 0;
    hermes_view_styles_reset();
    memset(&s_ui, 0, sizeof(s_ui));
}
