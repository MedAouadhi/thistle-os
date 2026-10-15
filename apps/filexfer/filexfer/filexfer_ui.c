/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — File Transfer's screen
 *
 * One page saying where to point a browser and the password to give it,
 * who has signed in and the last thing done, with two buttons:
 *
 *   ┌──────────────────────────────┐
 *   │ File Transfer                │  title bar
 *   │ On a computer or phone on    │
 *   │ this WiFi, open:             │
 *   │ http://thistledev.local      │
 *   │ or http://192.168.1.42       │
 *   │ Password                     │
 *   │ ABCD-EFGH                    │
 *   │ 1 signed in                  │
 *   │ Saved todo.txt               │
 *   │ [New password]     [Stop]    │
 *   └──────────────────────────────┘
 *
 * A timer asks the server how it is once a second, and lets it follow WiFi
 * (xfer_server_poll()). A label is set only when its text changes: the
 * e-paper redraws whatever is set.
 */
#include "filexfer/filexfer_app.h"
#include "filexfer/xfer_server.h"

#include "thistle/app_manager.h"
#include "thistle/wifi_manager.h"
#include "ui/theme.h"

#include "esp_log.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define TICK_MS   1000
#define TITLE_H   30
#define PAD       8
#define SETTINGS_ID "com.thistle.settings"

typedef enum {
    ACT_NONE,
    ACT_NEW_PASSWORD,
    ACT_START,
    ACT_SETTINGS,
    ACT_STOP,
} action_t;

static struct {
    lv_obj_t   *root;
    lv_obj_t   *heading;
    lv_obj_t   *url;
    lv_obj_t   *detail;
    lv_obj_t   *pw_caption;
    lv_obj_t   *password;
    lv_obj_t   *who;
    lv_obj_t   *activity;
    lv_obj_t   *buttons[2];
    lv_obj_t   *button_labels[2];
    action_t    actions[2];
    lv_timer_t *tick;
    xfer_status_t shown;
    bool        drawn;
} s_ui;

static struct {
    lv_style_t page;
    lv_style_t bar;
    lv_style_t box;
    lv_style_t note;
    lv_style_t medium;
    lv_style_t big;
    lv_style_t button;
    lv_style_t button_pressed;
    lv_style_t focus_ring;
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

    lv_style_set_bg_color(&s_styles.bar, clr->surface);
    lv_style_set_bg_opa(&s_styles.bar, LV_OPA_COVER);
    lv_style_set_border_side(&s_styles.bar, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&s_styles.bar, clr->text);
    lv_style_set_border_width(&s_styles.bar, 1);
    lv_style_set_radius(&s_styles.bar, 0);
    lv_style_set_pad_hor(&s_styles.bar, PAD);
    lv_style_set_pad_ver(&s_styles.bar, 0);
    lv_style_set_text_font(&s_styles.bar, &lv_font_montserrat_18);

    lv_style_set_bg_opa(&s_styles.box, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_styles.box, 0);
    lv_style_set_radius(&s_styles.box, 0);
    lv_style_set_pad_all(&s_styles.box, 0);
    lv_style_set_pad_column(&s_styles.box, PAD);

    lv_style_set_text_color(&s_styles.note, clr->text_secondary);

    lv_style_set_text_font(&s_styles.medium, &lv_font_montserrat_18);
    lv_style_set_text_font(&s_styles.big, &lv_font_montserrat_22);
    lv_style_set_text_letter_space(&s_styles.big, 2);

    lv_style_set_bg_color(&s_styles.button, clr->bg);
    lv_style_set_bg_opa(&s_styles.button, LV_OPA_COVER);
    lv_style_set_border_color(&s_styles.button, clr->text);
    lv_style_set_border_width(&s_styles.button, 1);
    lv_style_set_radius(&s_styles.button, 4);
    lv_style_set_pad_hor(&s_styles.button, 10);
    lv_style_set_pad_ver(&s_styles.button, 6);
    lv_style_set_shadow_width(&s_styles.button, 0);
    lv_style_set_text_color(&s_styles.button, clr->text);

    /* E-paper has black and white only: pressed is inverted. */
    lv_style_set_bg_color(&s_styles.button_pressed, clr->text);
    lv_style_set_text_color(&s_styles.button_pressed, clr->bg);

    lv_style_set_outline_width(&s_styles.focus_ring, 2);
    lv_style_set_outline_color(&s_styles.focus_ring, clr->text);
    lv_style_set_outline_pad(&s_styles.focus_ring, 1);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                              */
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

static void set_button(int i, action_t action, const char *text)
{
    s_ui.actions[i] = action;
    set_shown(s_ui.buttons[i], action != ACT_NONE);
    if (action != ACT_NONE) set_text(s_ui.button_labels[i], text);
}

static void address(const xfer_status_t *st, const char *host, char *out, size_t cap)
{
    if (st->port == 80) snprintf(out, cap, "http://%s", host);
    else snprintf(out, cap, "http://%s:%u", host, st->port);
}

static void draw_running(const xfer_status_t *st)
{
    char url[64], detail[80], who[64];
    set_text(s_ui.heading, "On a computer or phone on this WiFi, open:");
    if (st->mdns) {
        address(st, XFER_HOSTNAME ".local", url, sizeof url);
        char ip[48];
        address(st, st->ip, ip, sizeof ip);
        snprintf(detail, sizeof detail, "or %s", ip);
    } else {
        address(st, st->ip, url, sizeof url);
        detail[0] = '\0';
    }
    set_text(s_ui.url, url);
    set_shown(s_ui.url, true);
    set_text(s_ui.detail, detail);
    set_shown(s_ui.detail, detail[0] != '\0');

    int n;
    if (st->sessions == 1) n = snprintf(who, sizeof who, "1 browser signed in");
    else if (st->sessions > 1) n = snprintf(who, sizeof who, "%d browsers signed in", st->sessions);
    else n = snprintf(who, sizeof who, "Nobody signed in yet");
    if (st->locked) {
        snprintf(who + n, sizeof who - (size_t)n, "; sign-ins paused after wrong passwords");
    } else if (st->failed_logins == 1) {
        snprintf(who + n, sizeof who - (size_t)n, "; 1 wrong password");
    } else if (st->failed_logins > 1) {
        snprintf(who + n, sizeof who - (size_t)n, "; %u wrong passwords", (unsigned)st->failed_logins);
    }
    set_text(s_ui.who, who);
    set_shown(s_ui.who, true);
    set_text(s_ui.activity, st->activity);
    set_shown(s_ui.activity, st->activity[0] != '\0');

    set_button(0, ACT_NEW_PASSWORD, "New password");
    set_button(1, ACT_STOP, "Stop");
}

static void draw(const xfer_status_t *st)
{
    bool password = st->password[0] && (st->state == XFER_RUNNING || st->state == XFER_NO_WIFI);
    set_text(s_ui.password, st->password);
    set_shown(s_ui.pw_caption, password);
    set_shown(s_ui.password, password);

    switch (st->state) {
    case XFER_RUNNING:
        draw_running(st);
        return;

    case XFER_NO_WIFI: {
        bool off = !wifi_manager_is_enabled();
        set_text(s_ui.heading, off ? "WiFi is off" : "Waiting for WiFi");
        set_text(s_ui.detail, off ? "Switch WiFi on in the quick panel or in Settings. "
                                    "Files are shared as soon as it is up."
                                  : "Connect to a network in Settings > WiFi. "
                                    "Files are shared as soon as it is up.");
        set_button(0, ACT_SETTINGS, "Settings");
        set_button(1, ACT_STOP, "Stop");
        break;
    }
    case XFER_FAILED:
        set_text(s_ui.heading, "Could not share files");
        set_text(s_ui.detail, st->error);
        set_button(0, ACT_START, "Try again");
        set_button(1, ACT_NONE, "");
        break;

    case XFER_OFF:
    default:
        set_text(s_ui.heading, st->idle_stopped ? "Stopped after 15 minutes unused" : "Stopped");
        set_text(s_ui.detail, "Nothing is shared. Start again to share files, with a new password.");
        set_button(0, ACT_START, "Start");
        set_button(1, ACT_NONE, "");
        break;
    }
    set_shown(s_ui.url, false);
    set_shown(s_ui.detail, true);
    set_shown(s_ui.who, false);
    set_shown(s_ui.activity, false);
}

static void refresh(void)
{
    xfer_status_t st;
    xfer_server_status(&st);
    if (s_ui.drawn && memcmp(&st, &s_ui.shown, sizeof st) == 0) return;
    draw(&st);
    s_ui.shown = st;
    s_ui.drawn = true;
}

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    xfer_server_poll();
    refresh();
}

/* ------------------------------------------------------------------ */
/* Buttons                                                              */
/* ------------------------------------------------------------------ */

static void button_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    switch (s_ui.actions[i]) {
    case ACT_NEW_PASSWORD:
        xfer_server_new_password();
        break;
    case ACT_START:
        xfer_server_start();
        break;
    case ACT_STOP:
        xfer_server_stop();
        break;
    case ACT_SETTINGS:
        /* Leaving destroys this app, which stops the server: coming back
         * starts it again. */
        app_manager_launch(SETTINGS_ID);
        return;
    case ACT_NONE:
        return;
    }
    refresh();
}

/* ------------------------------------------------------------------ */
/* Making and unmaking                                                  */
/* ------------------------------------------------------------------ */

static lv_obj_t *label(lv_obj_t *parent, const lv_style_t *style, bool wrap)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    if (style) lv_obj_add_style(l, (lv_style_t *)style, LV_PART_MAIN);
    if (wrap) {
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, int i)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &s_styles.button, LV_PART_MAIN);
    lv_obj_add_style(b, &s_styles.button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(b, &s_styles.focus_ring, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(b, button_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    s_ui.button_labels[i] = lv_label_create(b);
    lv_label_set_text(s_ui.button_labels[i], "");
    lv_obj_center(s_ui.button_labels[i]);

    lv_group_t *grp = lv_group_get_default();
    if (grp) lv_group_add_obj(grp, b);
    return b;
}

esp_err_t filexfer_ui_create(lv_obj_t *parent)
{
    if (!parent) parent = lv_screen_active();
    memset(&s_ui, 0, sizeof s_ui);
    styles_init();

    s_ui.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.root);
    lv_obj_add_style(s_ui.root, &s_styles.page, LV_PART_MAIN);
    lv_obj_set_size(s_ui.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_ui.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, &s_styles.bar, LV_PART_MAIN);
    lv_obj_set_size(bar, LV_PCT(100), TITLE_H);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "File Transfer");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    /* Scrolls on a screen too short for it all. */
    lv_obj_t *body = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(body);
    lv_obj_add_style(body, &s_styles.box, LV_PART_MAIN);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_pad_all(body, PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(body, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    s_ui.heading = label(body, &s_styles.note, true);
    s_ui.url = label(body, &s_styles.medium, true);
    s_ui.detail = label(body, NULL, true);

    s_ui.pw_caption = label(body, &s_styles.note, false);
    lv_label_set_text(s_ui.pw_caption, "Password");
    lv_obj_set_style_pad_top(s_ui.pw_caption, 6, LV_PART_MAIN);
    s_ui.password = label(body, &s_styles.big, false);

    s_ui.who = label(body, &s_styles.note, true);
    lv_obj_set_style_pad_top(s_ui.who, 6, LV_PART_MAIN);
    s_ui.activity = label(body, &s_styles.note, true);

    lv_obj_t *row = lv_obj_create(body);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &s_styles.box, LV_PART_MAIN);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(row, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 2; i++) s_ui.buttons[i] = button(row, i);

    refresh();
    s_ui.tick = lv_timer_create(tick_cb, TICK_MS, NULL);
    return ESP_OK;
}

void filexfer_ui_destroy(void)
{
    if (s_ui.tick) lv_timer_delete(s_ui.tick);
    if (s_ui.root) lv_obj_delete(s_ui.root);
    memset(&s_ui, 0, sizeof s_ui);
    styles_reset();
}
