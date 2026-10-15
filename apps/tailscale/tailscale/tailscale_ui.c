/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Tailscale's screen
 *
 * One page saying what the tailnet is doing, with what it needs from the
 * user, and two buttons:
 *
 *   ┌──────────────────────────────┐
 *   │ Tailscale                    │  title bar
 *   │ Sign in                      │
 *   │ ┌────────┐                   │
 *   │ │ QR     │                   │  only while a sign-in waits
 *   │ └────────┘                   │
 *   │ login.tailscale.com/a/1f2e3d │
 *   │ Scan it with a phone and ... │
 *   │ [Switch off]                 │
 *   └──────────────────────────────┘
 *
 *   │ Connected                    │
 *   │ thistle-a1b2c3, 100.64.0.5   │
 *   │ On your tailnet              │
 *   │ homelab        100.64.0.2    │
 *   │ laptop         100.64.0.3 relay
 *   │ [Switch off]    [Sign out]   │
 *
 * A timer asks the kernel how it is once a second. A label is set only when
 * its text changes: the e-paper redraws whatever is set.
 */
#include "tailscale/tailscale_app.h"

#include "thistle/tailnet.h"
#include "ui/theme.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define TICK_MS     1000
#define TITLE_H     30
#define PAD         8
#define QR_SIZE     120
#define MAX_PEERS   16
/* A second press of Sign out within this long signs out. */
#define CONFIRM_MS  5000

typedef enum {
    ACT_NONE,
    ACT_ON,
    ACT_OFF,
    ACT_RETRY,
    ACT_SIGN_OUT,
} action_t;

static struct {
    lv_obj_t   *root;
    lv_obj_t   *heading;
    lv_obj_t   *detail;
    lv_obj_t   *qr;
    lv_obj_t   *url;
    lv_obj_t   *note;
    lv_obj_t   *peers_caption;
    lv_obj_t   *peers;
    lv_obj_t   *buttons[2];
    lv_obj_t   *button_labels[2];
    action_t    actions[2];
    lv_timer_t *tick;
    tailnet_status_t shown;
    char        qr_url[sizeof(((tailnet_status_t *)0)->auth_url)];
    char        peer_text[MAX_PEERS * 48];
    uint32_t    confirm_until;
    bool        drawn;
} s_ui;

static struct {
    lv_style_t page;
    lv_style_t bar;
    lv_style_t box;
    lv_style_t note;
    lv_style_t medium;
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

static bool confirming(void)
{
    return s_ui.confirm_until && (int32_t)(s_ui.confirm_until - lv_tick_get()) > 0;
}

/* The URL to sign in at, as a QR code and as text without its scheme, which
 * is what a person would type. */
static void show_sign_in(const char *url)
{
    if (strcmp(url, s_ui.qr_url) != 0) {
        lv_qrcode_update(s_ui.qr, url, strlen(url));
        strlcpy(s_ui.qr_url, url, sizeof s_ui.qr_url);
    }
    const char *bare = strstr(url, "://");
    set_text(s_ui.url, bare ? bare + 3 : url);
}

/* One line a peer: its first name, its address, and whether a relay
 * carries it. */
static const char *peer_lines(void)
{
    char *out = s_ui.peer_text;
    size_t cap = sizeof s_ui.peer_text, used = 0;
    out[0] = '\0';
    int n = tailnet_peer_count();
    if (n > MAX_PEERS) n = MAX_PEERS;
    for (int i = 0; i < n && used < cap; i++) {
        tailnet_peer_t p;
        if (tailnet_get_peer(i, &p) != ESP_OK) continue;
        char *dot = strchr(p.name, '.');
        if (dot) *dot = '\0';
        int w = snprintf(out + used, cap - used, "%s%-14s %s%s", used ? "\n" : "", p.name, p.ip,
                         p.direct ? "" : " relay");
        if (w < 0) break;
        used += (size_t)w < cap - used ? (size_t)w : cap - used - 1;
    }
    return out;
}

static void draw(const tailnet_status_t *st)
{
    char line[128];
    bool sign_in = st->state == TAILNET_NEEDS_LOGIN && st->auth_url[0];
    bool connected = st->state == TAILNET_CONNECTED;

    set_shown(s_ui.qr, sign_in);
    set_shown(s_ui.url, sign_in);
    if (sign_in) show_sign_in(st->auth_url);

    const char *note = "";
    switch (st->state) {
    case TAILNET_UNAVAILABLE:
        set_text(s_ui.heading, "Not on this board");
        set_text(s_ui.detail, "Tailscale needs an ESP32-S3 with PSRAM.");
        break;
    case TAILNET_OFF:
        set_text(s_ui.heading, "Off");
        set_text(s_ui.detail, "Switch on to join your tailnet: its devices and services are then "
                              "reachable from this one, by name.");
        if (!st->joined) {
            note = "The first time, you sign in with a QR code from your phone.";
        }
        break;
    case TAILNET_NO_NETWORK:
        set_text(s_ui.heading, "Waiting for a network");
        set_text(s_ui.detail, "Tailscale joins as soon as WiFi or mobile data is up.");
        break;
    case TAILNET_NO_CLOCK:
        set_text(s_ui.heading, "Waiting for the clock");
        set_text(s_ui.detail, "The clock has to be set before joining. It is set from the "
                              "network shortly.");
        break;
    case TAILNET_CONNECTING:
        set_text(s_ui.heading, "Connecting");
        snprintf(line, sizeof line, "As %s", st->hostname);
        set_text(s_ui.detail, line);
        break;
    case TAILNET_NEEDS_LOGIN:
        set_text(s_ui.heading, "Sign in");
        set_text(s_ui.detail, sign_in ? "" : "Waiting for a sign-in link...");
        note = "Scan it with a phone and sign in to your tailnet. This device joins as yours.";
        break;
    case TAILNET_NEEDS_APPROVAL:
        set_text(s_ui.heading, "Waiting for approval");
        snprintf(line, sizeof line, "An admin of the tailnet has to approve %s in the admin "
                                    "console.", st->hostname);
        set_text(s_ui.detail, line);
        break;
    case TAILNET_CONNECTED:
        set_text(s_ui.heading, "Connected");
        snprintf(line, sizeof line, "%s, %s", st->hostname, st->ip);
        set_text(s_ui.detail, line);
        break;
    case TAILNET_FAILED:
    default:
        set_text(s_ui.heading, "Stopped");
        set_text(s_ui.detail, st->error[0] ? st->error : "Tailscale stopped.");
        break;
    }
    set_shown(s_ui.detail, lv_label_get_text(s_ui.detail)[0] != '\0');

    set_text(s_ui.note, note);
    set_shown(s_ui.note, note[0] != '\0');

    set_shown(s_ui.peers_caption, connected);
    set_shown(s_ui.peers, connected);
    if (connected) {
        const char *peers = peer_lines();
        set_text(s_ui.peers_caption, peers[0] ? "On your tailnet" : "Nothing else on your tailnet");
        set_text(s_ui.peers, peers);
    }

    const char *sign_out = confirming() ? "Sure? Sign out" : "Sign out";
    switch (st->state) {
    case TAILNET_UNAVAILABLE:
        set_button(0, ACT_NONE, "");
        set_button(1, ACT_NONE, "");
        break;
    case TAILNET_OFF:
        set_button(0, ACT_ON, "Switch on");
        set_button(1, st->joined ? ACT_SIGN_OUT : ACT_NONE, sign_out);
        break;
    case TAILNET_FAILED:
        set_button(0, ACT_RETRY, "Try again");
        set_button(1, ACT_OFF, "Switch off");
        break;
    default:
        set_button(0, ACT_OFF, "Switch off");
        set_button(1, st->joined && st->state != TAILNET_NEEDS_LOGIN ? ACT_SIGN_OUT : ACT_NONE,
                   sign_out);
        break;
    }
}

static void refresh(bool force)
{
    tailnet_status_t st;
    if (tailnet_get_status(&st) != ESP_OK) return;
    /* Peers come and go without the status changing, so a connected page is
     * looked at each time; set_text() keeps the e-paper still if nothing
     * moved. */
    if (!force && s_ui.drawn && st.state != TAILNET_CONNECTED &&
        memcmp(&st, &s_ui.shown, sizeof st) == 0) {
        return;
    }
    draw(&st);
    s_ui.shown = st;
    s_ui.drawn = true;
}

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    bool was_confirming = s_ui.confirm_until != 0;
    if (was_confirming && !confirming()) s_ui.confirm_until = 0;
    refresh(was_confirming && !s_ui.confirm_until);
}

/* ------------------------------------------------------------------ */
/* Buttons                                                              */
/* ------------------------------------------------------------------ */

static void button_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    switch (s_ui.actions[i]) {
    case ACT_ON:
    case ACT_RETRY:
        tailnet_set_enabled(true);
        break;
    case ACT_OFF:
        tailnet_set_enabled(false);
        break;
    case ACT_SIGN_OUT:
        if (!confirming()) {
            s_ui.confirm_until = lv_tick_get() + CONFIRM_MS;
        } else {
            s_ui.confirm_until = 0;
            tailnet_forget();
        }
        break;
    case ACT_NONE:
        return;
    }
    refresh(true);
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

esp_err_t tailscale_ui_create(lv_obj_t *parent)
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
    lv_label_set_text(title, "Tailscale");
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

    s_ui.heading = label(body, &s_styles.medium, true);
    s_ui.detail = label(body, NULL, true);

    const theme_colors_t *clr = theme_get_colors();
    s_ui.qr = lv_qrcode_create(body);
    lv_qrcode_set_size(s_ui.qr, QR_SIZE);
    lv_qrcode_set_dark_color(s_ui.qr, clr->text);
    lv_qrcode_set_light_color(s_ui.qr, clr->bg);
    lv_obj_set_style_pad_top(s_ui.qr, 4, LV_PART_MAIN);
    lv_obj_add_flag(s_ui.qr, LV_OBJ_FLAG_HIDDEN);

    s_ui.url = label(body, NULL, true);
    lv_obj_add_flag(s_ui.url, LV_OBJ_FLAG_HIDDEN);
    s_ui.note = label(body, &s_styles.note, true);
    lv_obj_set_style_pad_top(s_ui.note, 4, LV_PART_MAIN);

    s_ui.peers_caption = label(body, &s_styles.note, false);
    lv_obj_set_style_pad_top(s_ui.peers_caption, 6, LV_PART_MAIN);
    s_ui.peers = label(body, NULL, true);

    lv_obj_t *row = lv_obj_create(body);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &s_styles.box, LV_PART_MAIN);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(row, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 2; i++) s_ui.buttons[i] = button(row, i);

    refresh(true);
    s_ui.tick = lv_timer_create(tick_cb, TICK_MS, NULL);
    return ESP_OK;
}

void tailscale_ui_destroy(void)
{
    if (s_ui.tick) lv_timer_delete(s_ui.tick);
    if (s_ui.root) lv_obj_delete(s_ui.root);
    memset(&s_ui, 0, sizeof s_ui);
    styles_reset();
}
