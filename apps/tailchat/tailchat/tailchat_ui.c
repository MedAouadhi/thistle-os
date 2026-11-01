/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Chat (Tailnet chat) screens
 *
 * Three tabs, and a page of detail beneath each:
 *
 *   ┌──────────────────────────────┐
 *   │ [Chats] [Contacts] [Requests]│  the tabs; Requests counts what waits
 *   │ Tailscale is off. ...        │  only while chat is unavailable
 *   │ laptop                       │  one lv_table per list
 *   │ Hello from the laptop        │
 *   │ phone                        │
 *   └──────────────────────────────┘
 *
 * - Chats: each contact with messages, its latest; a row opens the
 *   conversation, its latest 20 messages, one lv_table.
 * - Contacts: each contact, flagged if another key came under its name or
 *   address; this device's own fingerprint beneath. A row opens the
 *   contact: its fingerprint, Rename, Accept new key, Remove.
 * - Requests: each contact request, its name, fingerprint and latest
 *   message; a row opens it: what it held, Accept and Decline.
 *
 * The app reads and asks the kernel (thistle/tailnet_chat.h) and keeps
 * nothing of its own. A timer looks at tailnet_chat_generation() twice a
 * second and draws the page again only when it changed: on e-paper every
 * redraw is a refresh. Every page change from a click goes through
 * lv_async_call(), as it deletes the widget whose event is running.
 */
#include "tailchat/tailchat_app.h"
#include "tailchat/tailchat_core.h"

#include "thistle/tailnet_chat.h"
#include "hal/board.h"
#include "ui/manager.h"
#include "ui/theme.h"
#include "ui/toast.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "tailchat";

#define TAB_H      32
#define EDGE       6
#define ROWS_MAX   32   /* contacts and requests are capped below this by the kernel */
#define DETAIL_MAX 40   /* characters of a message shown in a list row */
#define LINE_MAX   200  /* characters of a message shown in a conversation */

typedef enum {
    PAGE_CHATS,
    PAGE_CONTACTS,
    PAGE_REQUESTS,
    PAGE_CONVERSATION,
    PAGE_CONTACT,
    PAGE_REQUEST,
} page_t;

/* What a page reads from the kernel, in PSRAM while the app is open. */
typedef struct {
    char keys[ROWS_MAX][65];       /* the key behind each list row */
    char text[4097];               /* one message's text */
    char row[640];
    tailnet_chat_contact_t contact;
    tailnet_chat_request_t request;
} store_t;

static struct {
    lv_obj_t   *root;
    lv_obj_t   *tabs[3];
    lv_obj_t   *tab_labels[3];
    lv_obj_t   *note;
    lv_obj_t   *body;
    lv_obj_t   *table;
    lv_obj_t   *field;       /* the contact page's name */
    lv_obj_t   *remove;      /* the contact page's Remove */
    lv_timer_t *timer;
    store_t    *store;
    page_t      page;
    page_t      next_page;
    char        key[65];     /* the detail page's chat peer */
    char        next_key[65];
    int         rows;
    uint32_t    generation;
    bool        confirm_remove;
    bool        epaper;
} s_ui;

static struct {
    lv_style_t page;
    lv_style_t tabs;
    lv_style_t tab;
    lv_style_t tab_checked;
    lv_style_t note;
    lv_style_t title;
    lv_style_t body;
    lv_style_t table;
    lv_style_t cell;
    lv_style_t button;
    lv_style_t button_pressed;
    lv_style_t focus_ring;
    lv_style_t warn;
} s_styles;
static bool s_styles_made;

/* ------------------------------------------------------------------ */
/* Styles                                                               */
/* ------------------------------------------------------------------ */

static void styles_reset(void)
{
    if (!s_styles_made) return;
    lv_style_t *all = (lv_style_t *)&s_styles;
    for (size_t i = 0; i < sizeof s_styles / sizeof(lv_style_t); i++) lv_style_reset(&all[i]);
    s_styles_made = false;
}

static void styles_init(void)
{
    styles_reset();
    lv_style_t *all = (lv_style_t *)&s_styles;
    for (size_t i = 0; i < sizeof s_styles / sizeof(lv_style_t); i++) lv_style_init(&all[i]);
    s_styles_made = true;
    const theme_colors_t *clr = theme_get_colors();

    lv_style_set_bg_color(&s_styles.page, clr->bg);
    lv_style_set_bg_opa(&s_styles.page, LV_OPA_COVER);
    lv_style_set_border_width(&s_styles.page, 0);
    lv_style_set_radius(&s_styles.page, 0);
    lv_style_set_pad_all(&s_styles.page, 0);
    lv_style_set_text_color(&s_styles.page, clr->text);
    lv_style_set_text_font(&s_styles.page, &lv_font_montserrat_14);

    lv_style_set_bg_color(&s_styles.tabs, clr->surface);
    lv_style_set_bg_opa(&s_styles.tabs, LV_OPA_COVER);
    lv_style_set_border_side(&s_styles.tabs, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&s_styles.tabs, clr->text);
    lv_style_set_border_width(&s_styles.tabs, 1);
    lv_style_set_pad_all(&s_styles.tabs, 3);
    lv_style_set_pad_column(&s_styles.tabs, 3);

    lv_style_set_bg_color(&s_styles.tab, clr->bg);
    lv_style_set_bg_opa(&s_styles.tab, LV_OPA_COVER);
    lv_style_set_border_color(&s_styles.tab, clr->text);
    lv_style_set_border_width(&s_styles.tab, 1);
    lv_style_set_radius(&s_styles.tab, 4);
    lv_style_set_pad_all(&s_styles.tab, 2);
    lv_style_set_text_color(&s_styles.tab, clr->text);
    lv_style_set_text_font(&s_styles.tab, &lv_font_montserrat_14);

    /* The tab shown has a heavy border: black and white only on e-paper,
     * and the theme gives labels their own colour, so nothing is inverted. */
    lv_style_set_border_width(&s_styles.tab_checked, 3);

    lv_style_set_text_color(&s_styles.note, clr->text_secondary);
    lv_style_set_pad_hor(&s_styles.note, EDGE);
    lv_style_set_pad_ver(&s_styles.note, 4);

    lv_style_set_text_font(&s_styles.title, &lv_font_montserrat_18);
    lv_style_set_pad_hor(&s_styles.title, EDGE);
    lv_style_set_pad_top(&s_styles.title, 4);

    lv_style_set_pad_all(&s_styles.body, 0);
    lv_style_set_pad_row(&s_styles.body, 4);
    lv_style_set_pad_bottom(&s_styles.body, 6);

    lv_style_set_bg_opa(&s_styles.table, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_styles.table, 0);
    lv_style_set_pad_hor(&s_styles.table, EDGE);
    lv_style_set_pad_ver(&s_styles.table, 2);

    lv_style_set_bg_color(&s_styles.cell, clr->bg);
    lv_style_set_bg_opa(&s_styles.cell, LV_OPA_COVER);
    lv_style_set_border_width(&s_styles.cell, 0);
    lv_style_set_pad_hor(&s_styles.cell, 4);
    lv_style_set_pad_ver(&s_styles.cell, 6);
    lv_style_set_text_color(&s_styles.cell, clr->text);
    lv_style_set_text_font(&s_styles.cell, &lv_font_montserrat_14);

    lv_style_set_bg_color(&s_styles.button, clr->bg);
    lv_style_set_bg_opa(&s_styles.button, LV_OPA_COVER);
    lv_style_set_border_color(&s_styles.button, clr->text);
    lv_style_set_border_width(&s_styles.button, 1);
    lv_style_set_radius(&s_styles.button, 4);
    lv_style_set_pad_hor(&s_styles.button, 10);
    lv_style_set_pad_ver(&s_styles.button, 6);
    lv_style_set_text_color(&s_styles.button, clr->text);

    lv_style_set_border_width(&s_styles.button_pressed, 3);

    lv_style_set_outline_width(&s_styles.focus_ring, 2);
    lv_style_set_outline_color(&s_styles.focus_ring, clr->primary);
    lv_style_set_outline_pad(&s_styles.focus_ring, 1);

    lv_style_set_text_color(&s_styles.warn, clr->text);
    lv_style_set_border_side(&s_styles.warn, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_width(&s_styles.warn, 3);
    lv_style_set_border_color(&s_styles.warn, clr->text);
    lv_style_set_pad_left(&s_styles.warn, EDGE);
    lv_style_set_margin_left(&s_styles.warn, EDGE);
}

/* ------------------------------------------------------------------ */
/* Little things                                                        */
/* ------------------------------------------------------------------ */

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void set_shown(lv_obj_t *obj, bool shown)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == !shown) return;
    if (shown) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void join_group(lv_obj_t *obj)
{
    lv_group_t *group = lv_group_get_default();
    if (group) lv_group_add_obj(group, obj);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, lv_style_t *style)
{
    lv_obj_t *l = lv_label_create(parent);
    if (style) lv_obj_add_style(l, style, LV_PART_MAIN);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &s_styles.button, LV_PART_MAIN);
    lv_obj_add_style(b, &s_styles.button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(b, &s_styles.focus_ring, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    join_group(b);
    return b;
}

static lv_obj_t *button_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_hor(row, EDGE, 0);
    lv_obj_set_style_pad_gap(row, 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/* ------------------------------------------------------------------ */
/* Going between pages                                                  */
/* ------------------------------------------------------------------ */

static void show_page(void);

static void go_async(void *arg)
{
    (void)arg;
    if (!s_ui.root) return;
    s_ui.page = s_ui.next_page;
    memcpy(s_ui.key, s_ui.next_key, sizeof s_ui.key);
    s_ui.confirm_remove = false;
    show_page();
}

static void go(page_t page, const char *key)
{
    s_ui.next_page = page;
    snprintf(s_ui.next_key, sizeof s_ui.next_key, "%s", key ? key : "");
    lv_async_call(go_async, NULL);
}

static page_t parent_of(page_t page)
{
    switch (page) {
    case PAGE_CONVERSATION: return PAGE_CHATS;
    case PAGE_CONTACT:      return PAGE_CONTACTS;
    case PAGE_REQUEST:      return PAGE_REQUESTS;
    default:                return page;
    }
}

static bool back_cb(void)
{
    if (s_ui.page <= PAGE_REQUESTS) return false;
    go(parent_of(s_ui.page), NULL);
    return true;
}

static void tab_cb(lv_event_t *e)
{
    page_t page = (page_t)(intptr_t)lv_event_get_user_data(e);
    go(page, NULL);
}

/* The list row chosen: by a tap or Enter, not the selection moving. */
static void table_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD && lv_indev_get_key(indev) != LV_KEY_ENTER) {
        return;
    }
    uint32_t row = LV_TABLE_CELL_NONE, col = LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(s_ui.table, &row, &col);
    if (row >= (uint32_t)s_ui.rows) return;
    page_t to = s_ui.page == PAGE_CHATS ? PAGE_CONVERSATION
              : s_ui.page == PAGE_CONTACTS ? PAGE_CONTACT
                                           : PAGE_REQUEST;
    go(to, s_ui.store->keys[row]);
}

static lv_obj_t *make_table(int rows, bool clickable)
{
    lv_obj_t *t = lv_table_create(s_ui.body);
    lv_obj_remove_style_all(t);
    lv_obj_add_style(t, &s_styles.table, LV_PART_MAIN);
    lv_obj_add_style(t, &s_styles.cell, LV_PART_ITEMS);
    lv_obj_add_style(t, &s_styles.focus_ring, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_width(t, LV_PCT(100));
    lv_table_set_column_count(t, 1);
    lv_obj_update_layout(s_ui.body);
    int32_t width = lv_obj_get_content_width(s_ui.body);
    lv_table_set_column_width(t, 0, (width > 0 ? width : 240) - 2 * EDGE);
    lv_table_set_row_count(t, (uint32_t)rows);
    theme_rule_table_rows(t);
    if (clickable) {
        lv_obj_add_event_cb(t, table_cb, LV_EVENT_VALUE_CHANGED, NULL);
        join_group(t);
    } else {
        lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    }
    return t;
}

/* ------------------------------------------------------------------ */
/* The lists                                                            */
/* ------------------------------------------------------------------ */

static void empty(const char *text)
{
    lv_obj_t *l = label(s_ui.body, text, &s_styles.note);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(l, 24, 0);
}

static void own_fingerprint(void)
{
    tailnet_chat_status_t st;
    if (tailnet_chat_get_status(&st) != ESP_OK || !st.fingerprint[0]) return;
    snprintf(s_ui.store->row, sizeof s_ui.store->row, "This device: %s\n%s", st.name, st.fingerprint);
    label(s_ui.body, s_ui.store->row, &s_styles.note);
}

static void show_chats(void)
{
    int n = tailnet_chat_contact_count();
    s_ui.rows = 0;
    for (int i = 0; i < n && s_ui.rows < ROWS_MAX; i++) {
        tailnet_chat_contact_t *c = &s_ui.store->contact;
        if (tailnet_chat_get_contact(i, c) != ESP_OK) continue;
        int count = tailnet_chat_message_count(c->key);
        if (count <= 0) continue;
        tailnet_chat_message_t m;
        tailnet_chat_get_message(c->key, count - 1, &m, s_ui.store->text, sizeof s_ui.store->text);
        if (!s_ui.table) s_ui.table = make_table(n, true);
        tailchat_row(s_ui.store->row, sizeof s_ui.store->row, c->name, s_ui.store->text, DETAIL_MAX);
        lv_table_set_cell_value(s_ui.table, (uint32_t)s_ui.rows, 0, s_ui.store->row);
        memcpy(s_ui.store->keys[s_ui.rows++], c->key, 65);
    }
    if (s_ui.table) lv_table_set_row_count(s_ui.table, (uint32_t)s_ui.rows);
    else empty("No conversations yet.\nA chat peer's first message comes as a contact request.");
}

static void show_contacts(void)
{
    int n = tailnet_chat_contact_count();
    s_ui.rows = 0;
    if (n > 0) s_ui.table = make_table(n < ROWS_MAX ? n : ROWS_MAX, true);
    for (int i = 0; i < n && s_ui.rows < ROWS_MAX; i++) {
        tailnet_chat_contact_t *c = &s_ui.store->contact;
        if (tailnet_chat_get_contact(i, c) != ESP_OK) continue;
        const char *detail = c->flagged ? "Another key came under this name" : c->fingerprint;
        tailchat_row(s_ui.store->row, sizeof s_ui.store->row, c->name, detail, DETAIL_MAX);
        lv_table_set_cell_value(s_ui.table, (uint32_t)s_ui.rows, 0, s_ui.store->row);
        memcpy(s_ui.store->keys[s_ui.rows++], c->key, 65);
    }
    if (!n) empty("No contacts yet.");
    own_fingerprint();
}

static void show_requests(void)
{
    int n = tailnet_chat_request_count();
    s_ui.rows = 0;
    if (n > 0) s_ui.table = make_table(n < ROWS_MAX ? n : ROWS_MAX, true);
    for (int i = 0; i < n && s_ui.rows < ROWS_MAX; i++) {
        tailnet_chat_request_t *r = &s_ui.store->request;
        if (tailnet_chat_get_request(i, r) != ESP_OK) continue;
        char title[100];
        snprintf(title, sizeof title, "%s\n%s", r->name, r->fingerprint);
        tailchat_row(s_ui.store->row, sizeof s_ui.store->row, title, r->preview, DETAIL_MAX);
        lv_table_set_cell_value(s_ui.table, (uint32_t)s_ui.rows, 0, s_ui.store->row);
        memcpy(s_ui.store->keys[s_ui.rows++], r->key, 65);
    }
    if (!n) empty("No contact requests.");
}

/* ------------------------------------------------------------------ */
/* The detail pages                                                     */
/* ------------------------------------------------------------------ */

static bool find_contact(const char *key, tailnet_chat_contact_t *out)
{
    int n = tailnet_chat_contact_count();
    for (int i = 0; i < n; i++) {
        if (tailnet_chat_get_contact(i, out) == ESP_OK && strcmp(out->key, key) == 0) return true;
    }
    return false;
}

static bool find_request(const char *key, tailnet_chat_request_t *out)
{
    int n = tailnet_chat_request_count();
    for (int i = 0; i < n; i++) {
        if (tailnet_chat_get_request(i, out) == ESP_OK && strcmp(out->key, key) == 0) return true;
    }
    return false;
}

static void show_conversation(void)
{
    tailnet_chat_contact_t *c = &s_ui.store->contact;
    if (!find_contact(s_ui.key, c)) {
        go(PAGE_CHATS, NULL);
        return;
    }
    label(s_ui.body, c->name, &s_styles.title);
    int n = tailnet_chat_message_count(c->key);
    s_ui.rows = n;
    if (n <= 0) {
        empty("No messages yet.");
        return;
    }
    s_ui.table = make_table(n, false);
    for (int i = 0; i < n; i++) {
        tailnet_chat_message_t m;
        if (tailnet_chat_get_message(c->key, i, &m, s_ui.store->text, sizeof s_ui.store->text) != ESP_OK) {
            continue;
        }
        tailchat_message_row(s_ui.store->row, sizeof s_ui.store->row, m.mine, c->name, s_ui.store->text, LINE_MAX);
        lv_table_set_cell_value(s_ui.table, (uint32_t)i, 0, s_ui.store->row);
    }
    lv_obj_update_layout(s_ui.body);
    lv_obj_scroll_to_y(s_ui.body, LV_COORD_MAX, LV_ANIM_OFF);
    ESP_LOGI(TAG, "conversation %s: %d messages", c->name, n);
}

static void rename_cb(lv_event_t *e)
{
    (void)e;
    const char *name = lv_textarea_get_text(s_ui.field);
    if (tailnet_chat_rename(s_ui.key, name) == ESP_OK) {
        ESP_LOGI(TAG, "renamed to %s", name);
        toast_info("Renamed");
    } else {
        toast_warn("A name needs a letter in it");
    }
}

static void new_key_cb(lv_event_t *e)
{
    (void)e;
    if (tailnet_chat_accept_new_key(s_ui.key) == ESP_OK) {
        ESP_LOGI(TAG, "accepted the new key");
        toast_info("New key accepted");
        go(PAGE_CONTACTS, NULL);
    }
}

static void remove_cb(lv_event_t *e)
{
    (void)e;
    if (!s_ui.confirm_remove) {
        s_ui.confirm_remove = true;
        set_text(lv_obj_get_child(s_ui.remove, 0), "Press again to remove");
        return;
    }
    if (tailnet_chat_remove(s_ui.key) == ESP_OK) {
        ESP_LOGI(TAG, "removed a contact");
        toast_info("Removed; it will have to ask again");
    }
    go(PAGE_CONTACTS, NULL);
}

static void show_contact(void)
{
    tailnet_chat_contact_t *c = &s_ui.store->contact;
    if (!find_contact(s_ui.key, c)) {
        go(PAGE_CONTACTS, NULL);
        return;
    }
    label(s_ui.body, c->name, &s_styles.title);
    snprintf(s_ui.store->row, sizeof s_ui.store->row, "Fingerprint\n%s\nSeen as %s%s\n%s",
             c->fingerprint, c->seen_name, c->connected ? ", connected" : "",
             tailchat_answer_text(c->accepted_by_them));
    label(s_ui.body, s_ui.store->row, &s_styles.note);

    if (c->flagged) {
        snprintf(s_ui.store->row, sizeof s_ui.store->row,
                 "Another key came under this name or address and was refused:\n%s\n"
                 "Accept it only if you know why the key changed, such as a reinstalled app.",
                 c->flagged_fingerprint);
        label(s_ui.body, s_ui.store->row, &s_styles.warn);
        button(button_row(s_ui.body), "Accept new key", new_key_cb);
    }

    s_ui.field = lv_textarea_create(s_ui.body);
    lv_textarea_set_one_line(s_ui.field, true);
    lv_textarea_set_max_length(s_ui.field, 64);
    lv_textarea_set_text(s_ui.field, c->name);
    lv_obj_set_width(s_ui.field, LV_PCT(100));
    lv_obj_set_style_margin_hor(s_ui.field, EDGE, 0);
    lv_obj_add_event_cb(s_ui.field, rename_cb, LV_EVENT_READY, NULL);
    join_group(s_ui.field);

    lv_obj_t *row = button_row(s_ui.body);
    button(row, "Rename", rename_cb);
    s_ui.remove = button(row, "Remove", remove_cb);
    ESP_LOGI(TAG, "contact %s %s%s", c->name, c->fingerprint, c->flagged ? ", flagged" : "");
}

static void accept_cb(lv_event_t *e)
{
    (void)e;
    if (tailnet_chat_accept(s_ui.key) == ESP_OK) {
        ESP_LOGI(TAG, "accepted the request");
        toast_info("Accepted");
    } else {
        toast_warn("Could not accept: too many contacts");
    }
    go(PAGE_REQUESTS, NULL);
}

static void decline_cb(lv_event_t *e)
{
    (void)e;
    if (tailnet_chat_decline(s_ui.key) == ESP_OK) {
        ESP_LOGI(TAG, "declined the request");
        toast_info("Declined");
    }
    go(PAGE_REQUESTS, NULL);
}

static void show_request(void)
{
    tailnet_chat_request_t *r = &s_ui.store->request;
    if (!find_request(s_ui.key, r)) {
        go(PAGE_REQUESTS, NULL);
        return;
    }
    label(s_ui.body, r->name, &s_styles.title);
    if (r->held == 1) {
        snprintf(s_ui.store->row, sizeof s_ui.store->row,
                 "Fingerprint\n%s\nwants to be a contact. It sent:", r->fingerprint);
    } else {
        snprintf(s_ui.store->row, sizeof s_ui.store->row,
                 "Fingerprint\n%s\nwants to be a contact. It sent %u messages; the latest:",
                 r->fingerprint, (unsigned)r->held);
    }
    label(s_ui.body, s_ui.store->row, &s_styles.note);
    /* What it held: the kernel keeps the latest; the preview is the last. */
    s_ui.table = make_table(1, false);
    tailchat_one_line(s_ui.store->row, sizeof s_ui.store->row, r->preview, LINE_MAX);
    lv_table_set_cell_value(s_ui.table, 0, 0, s_ui.store->row);

    lv_obj_t *row = button_row(s_ui.body);
    lv_obj_t *accept = button(row, "Accept", accept_cb);
    button(row, "Decline", decline_cb);
    lv_group_t *group = lv_group_get_default();
    if (group) lv_group_focus_obj(accept);
    ESP_LOGI(TAG, "request %s %s: %u held", r->name, r->fingerprint, (unsigned)r->held);
}

/* ------------------------------------------------------------------ */
/* Drawing the page                                                     */
/* ------------------------------------------------------------------ */

static const char *page_name(page_t p)
{
    static const char *names[] = {"chats", "contacts", "requests", "conversation", "contact", "request"};
    return names[p];
}

static void show_status(void)
{
    tailnet_chat_status_t st;
    if (tailnet_chat_get_status(&st) != ESP_OK) {
        memset(&st, 0, sizeof st);
        snprintf(st.why, sizeof st.why, "Tailnet chat has not started.");
    }
    bool listening = st.state == TAILNET_CHAT_LISTENING;
    if (!listening && strcmp(lv_label_get_text(s_ui.note), st.why) != 0) {
        ESP_LOGI(TAG, "unavailable: %s", st.why);
    }
    set_text(s_ui.note, listening ? "" : st.why);
    set_shown(s_ui.note, !listening);

    char text[24];
    for (int t = 0; t < 3; t++) {
        tailchat_tab_label((tailchat_tab_t)t, t == TAILCHAT_TAB_REQUESTS ? st.requests : 0, text, sizeof text);
        set_text(s_ui.tab_labels[t], text);
        if ((page_t)t == parent_of(s_ui.page)) lv_obj_add_state(s_ui.tabs[t], LV_STATE_CHECKED);
        else lv_obj_remove_state(s_ui.tabs[t], LV_STATE_CHECKED);
    }
}

static void show_page(void)
{
    s_ui.generation = tailnet_chat_generation();
    show_status();
    lv_obj_clean(s_ui.body);
    s_ui.table = NULL;
    s_ui.field = NULL;
    s_ui.remove = NULL;
    s_ui.rows = 0;
    switch (s_ui.page) {
    case PAGE_CHATS:        show_chats(); break;
    case PAGE_CONTACTS:     show_contacts(); break;
    case PAGE_REQUESTS:     show_requests(); break;
    case PAGE_CONVERSATION: show_conversation(); break;
    case PAGE_CONTACT:      show_contact(); break;
    case PAGE_REQUEST:      show_request(); break;
    }
    if (s_ui.page <= PAGE_REQUESTS) {
        ESP_LOGI(TAG, "page %s: %d rows", page_name(s_ui.page), s_ui.rows);
    }
}

/* Twice a second: draw again only if anything shown changed, and never
 * under the user's typing. */
static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (tailnet_chat_generation() == s_ui.generation) return;
    if (s_ui.page == PAGE_CONTACT) {
        s_ui.generation = tailnet_chat_generation();
        show_status();
        return;
    }
    show_page();
}

/* ------------------------------------------------------------------ */
/* Making and unmaking                                                  */
/* ------------------------------------------------------------------ */

static void make_tabs(void)
{
    lv_obj_t *bar = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, &s_styles.tabs, LV_PART_MAIN);
    lv_obj_set_size(bar, LV_PCT(100), TAB_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    for (int t = 0; t < 3; t++) {
        lv_obj_t *b = lv_button_create(bar);
        lv_obj_remove_style_all(b);
        lv_obj_add_style(b, &s_styles.tab, LV_PART_MAIN);
        lv_obj_add_style(b, &s_styles.tab_checked, LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_add_style(b, &s_styles.focus_ring, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
        lv_obj_set_height(b, LV_PCT(100));
        /* Requests is the widest, with its count: "Requests 12" */
        static const uint8_t grow[3] = {20, 23, 28};
        lv_obj_set_flex_grow(b, grow[t]);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)t);
        s_ui.tab_labels[t] = lv_label_create(b);
        lv_label_set_text(s_ui.tab_labels[t], "");
        lv_obj_center(s_ui.tab_labels[t]);
        s_ui.tabs[t] = b;
        join_group(b);
    }
}

esp_err_t tailchat_ui_create(lv_obj_t *parent)
{
    if (!parent) parent = lv_screen_active();
    memset(&s_ui, 0, sizeof s_ui);
    s_ui.store = heap_caps_malloc(sizeof *s_ui.store, MALLOC_CAP_SPIRAM);
    if (!s_ui.store) s_ui.store = malloc(sizeof *s_ui.store);
    if (!s_ui.store) return ESP_ERR_NO_MEM;

    const hal_registry_t *reg = hal_get_registry();
    s_ui.epaper = reg && reg->display && reg->display->type == HAL_DISPLAY_TYPE_EPAPER;
    styles_init();

    s_ui.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.root);
    lv_obj_add_style(s_ui.root, &s_styles.page, LV_PART_MAIN);
    lv_obj_set_size(s_ui.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_ui.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    make_tabs();
    s_ui.note = label(s_ui.root, "", &s_styles.note);

    s_ui.body = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(s_ui.body);
    lv_obj_add_style(s_ui.body, &s_styles.body, LV_PART_MAIN);
    lv_obj_set_width(s_ui.body, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.body, 1);
    lv_obj_set_flex_flow(s_ui.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_ui.body, LV_DIR_VER);

    ui_manager_set_back_handler(s_ui.root, back_cb);
    s_ui.page = PAGE_CHATS;
    show_page();
    s_ui.timer = lv_timer_create(timer_cb, 500, NULL);

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "LVGL pool %u KB free, largest block %u KB", (unsigned)(mon.free_size / 1024),
             (unsigned)(mon.free_biggest_size / 1024));
    return ESP_OK;
}

void tailchat_ui_destroy(void)
{
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    lv_async_call_cancel(go_async, NULL);
    if (s_ui.root) lv_obj_delete(s_ui.root);
    free(s_ui.store);
    memset(&s_ui, 0, sizeof s_ui);
    styles_reset();
}
