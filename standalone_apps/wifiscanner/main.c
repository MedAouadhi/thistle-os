// SPDX-License-Identifier: BSD-3-Clause
// WiFi Scanner — nearby networks, as a toolkit-neutral standalone app.
//
// Scan lists the networks found, strongest first, with their signal,
// channel and security, and which channels are busiest. A network's page
// joins it, with a password for a secured one. Scanning and joining block
// the screen for their few seconds, as Settings' do. Everything handed to
// the kernel is static: a device refuses pointers into internal RAM, where
// the UI task's stack is.

#include "thistle_app.h"

#include <stdio.h>
#include <string.h>

#define TAG "wifiscanner"
#define MAX_NETWORKS 16
#define PASSWORD_MAX 64
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107

static thistle_wifi_ap_t s_found[MAX_NETWORKS];
static int s_count;
static int s_selected = -1;
static char s_ssid[THISTLE_WIFI_SSID_MAX + 1];
static char s_password[PASSWORD_MAX + 1];

static thistle_widget_t s_list_screen = THISTLE_WIDGET_NONE;
static thistle_widget_t s_status = THISTLE_WIDGET_NONE;
static thistle_widget_t s_channels = THISTLE_WIDGET_NONE;
static thistle_widget_t s_list = THISTLE_WIDGET_NONE;
static thistle_widget_t s_rows[MAX_NETWORKS];

static thistle_widget_t s_detail_screen = THISTLE_WIDGET_NONE;
static thistle_widget_t s_detail_title = THISTLE_WIDGET_NONE;
static thistle_widget_t s_detail_info = THISTLE_WIDGET_NONE;
static thistle_widget_t s_password_field = THISTLE_WIDGET_NONE;
static thistle_widget_t s_connect = THISTLE_WIDGET_NONE;
static thistle_widget_t s_detail_status = THISTLE_WIDGET_NONE;

static const char *quality(int rssi)
{
    if (rssi >= -50) return "excellent";
    if (rssi >= -60) return "good";
    if (rssi >= -70) return "fair";
    return "weak";
}

static const char *name_of(const thistle_wifi_ap_t *ap)
{
    return ap->ssid[0] ? ap->ssid : "(hidden network)";
}

/* The three channels with the most networks on them. */
static void show_channels(void)
{
    int on[15] = {0};
    for (int i = 0; i < s_count; i++) {
        if (s_found[i].channel < 15) on[s_found[i].channel]++;
    }
    char text[80] = "Busiest channels:";
    size_t length = strlen(text);
    for (int shown = 0; shown < 3; shown++) {
        int best = 0;
        for (int channel = 1; channel < 15; channel++) {
            if (on[channel] > on[best]) best = channel;
        }
        if (on[best] == 0) break;
        length += snprintf(text + length, sizeof text - length, "%s %d (%d)", shown ? "," : "",
                           best, on[best]);
        on[best] = 0;
    }
    thistle_ui_set_text(s_channels, s_count ? text : "");
}

static void row_clicked(thistle_widget_t widget, int event, void *user_data);

static void show_list(void)
{
    for (int i = 0; i < MAX_NETWORKS; i++) {
        if (s_rows[i] != THISTLE_WIDGET_NONE) thistle_ui_destroy(s_rows[i]);
        s_rows[i] = THISTLE_WIDGET_NONE;
    }
    char text[96];
    for (int i = 0; i < s_count; i++) {
        const thistle_wifi_ap_t *ap = &s_found[i];
        snprintf(text, sizeof text, "%s\n%d dBm, %s   ch %u   %s", name_of(ap), ap->rssi,
                 quality(ap->rssi), (unsigned)ap->channel, ap->is_open ? "open" : "secured");
        s_rows[i] = thistle_ui_create_button(s_list, text);
        thistle_ui_set_size(s_rows[i], -1, 46);
        thistle_ui_set_bg_color(s_rows[i], thistle_ui_theme_surface());
        thistle_ui_set_text_color(s_rows[i], thistle_ui_theme_text());
        thistle_ui_set_font_size(s_rows[i], 14);
        thistle_ui_set_radius(s_rows[i], 6);
        /* The index rides in the pointer: rows are rebuilt with each scan. */
        thistle_ui_on_event(s_rows[i], THISTLE_EVENT_CLICK, row_clicked, (void *)(size_t)i);
    }
    show_channels();
}

static void scan(void)
{
    thistle_ui_set_text(s_status, "Scanning...");
    int found = thistle_wifi_scan(s_found, MAX_NETWORKS);
    char text[64];
    if (found < 0) {
        s_count = 0;
        if (-found == ESP_ERR_INVALID_STATE) {
            snprintf(text, sizeof text, "WiFi is off");
        } else if (-found == ESP_ERR_TIMEOUT) {
            snprintf(text, sizeof text, "WiFi is joining a network; try again");
        } else {
            snprintf(text, sizeof text, "Scan failed (0x%x)", -found);
        }
    } else {
        s_count = found;
        /* Strongest first. */
        for (int i = 1; i < s_count; i++) {
            thistle_wifi_ap_t ap = s_found[i];
            int j = i;
            for (; j > 0 && s_found[j - 1].rssi < ap.rssi; j--) s_found[j] = s_found[j - 1];
            s_found[j] = ap;
        }
        snprintf(text, sizeof text, "%d network%s found", s_count, s_count == 1 ? "" : "s");
    }
    thistle_log(TAG, text);
    thistle_ui_set_text(s_status, text);
    show_list();
}

static void scan_clicked(thistle_widget_t widget, int event, void *user_data)
{
    (void)widget;
    (void)event;
    (void)user_data;
    scan();
}

static void show_detail(int index)
{
    s_selected = index;
    const thistle_wifi_ap_t *ap = &s_found[index];
    char text[128];
    thistle_ui_set_text(s_detail_title, name_of(ap));
    snprintf(text, sizeof text, "Signal:  %d dBm, %s\nChannel:  %u\nSecurity:  %s", ap->rssi,
             quality(ap->rssi), (unsigned)ap->channel, ap->is_open ? "open" : "password");
    thistle_ui_set_text(s_detail_info, text);
    thistle_ui_set_text(s_password_field, "");
    thistle_ui_set_visible(s_password_field, !ap->is_open && ap->ssid[0]);
    thistle_ui_set_visible(s_connect, ap->ssid[0] != '\0');
    thistle_ui_set_text(s_detail_status, ap->ssid[0] ? "" : "A hidden network cannot be joined here");
    thistle_ui_set_visible(s_list_screen, false);
    thistle_ui_set_visible(s_detail_screen, true);
}

static void row_clicked(thistle_widget_t widget, int event, void *user_data)
{
    (void)widget;
    (void)event;
    int index = (int)(size_t)user_data;
    if (index >= 0 && index < s_count) show_detail(index);
}

static void back_clicked(thistle_widget_t widget, int event, void *user_data)
{
    (void)widget;
    (void)event;
    (void)user_data;
    s_selected = -1;
    thistle_ui_set_visible(s_detail_screen, false);
    thistle_ui_set_visible(s_list_screen, true);
}

static void connect_clicked(thistle_widget_t widget, int event, void *user_data)
{
    (void)widget;
    (void)event;
    (void)user_data;
    if (s_selected < 0 || s_selected >= s_count) return;
    const thistle_wifi_ap_t *ap = &s_found[s_selected];
    snprintf(s_ssid, sizeof s_ssid, "%s", ap->ssid);
    const char *typed = ap->is_open ? "" : thistle_ui_get_text(s_password_field);
    snprintf(s_password, sizeof s_password, "%s", typed ? typed : "");
    if (!ap->is_open && strlen(s_password) < 8) {
        thistle_ui_set_text(s_detail_status, "The password is at least 8 characters");
        return;
    }

    thistle_ui_set_text(s_detail_status, "Joining...");
    int err = thistle_wifi_connect(s_ssid, s_password);
    memset(s_password, 0, sizeof s_password);
    char text[96];
    if (err == 0) {
        snprintf(text, sizeof text, "Joined %s", s_ssid);
    } else {
        snprintf(text, sizeof text, "Could not join %s (0x%x)", s_ssid, err);
    }
    thistle_log(TAG, text);
    thistle_ui_set_text(s_detail_status, text);
}

static thistle_widget_t button(thistle_widget_t parent, const char *text, int width, bool primary)
{
    thistle_widget_t b = thistle_ui_create_button(parent, text);
    thistle_ui_set_size(b, width, 32);
    thistle_ui_set_radius(b, 6);
    thistle_ui_set_bg_color(b, primary ? thistle_ui_theme_primary() : thistle_ui_theme_surface());
    thistle_ui_set_text_color(b, primary ? 0xFFFFFF : thistle_ui_theme_text());
    return b;
}

static thistle_widget_t screen(void)
{
    thistle_widget_t s = thistle_ui_create_container(thistle_ui_get_app_root());
    thistle_ui_set_size(s, -1, -1);
    thistle_ui_set_layout(s, THISTLE_LAYOUT_FLEX_COLUMN);
    thistle_ui_set_gap(s, 6);
    thistle_ui_set_padding(s, 8, 8, 8, 8);
    thistle_ui_set_bg_color(s, thistle_ui_theme_bg());
    return s;
}

static thistle_widget_t label(thistle_widget_t parent, const char *text, int size, uint32_t color)
{
    thistle_widget_t l = thistle_ui_create_label(parent, text);
    thistle_ui_set_font_size(l, size);
    thistle_ui_set_text_color(l, color);
    return l;
}

static int wifiscanner_on_create(void)
{
    thistle_log(TAG, "on_create");
    uint32_t text = thistle_ui_theme_text();
    uint32_t secondary = thistle_ui_theme_text_secondary();

    s_list_screen = screen();
    thistle_widget_t header = thistle_ui_create_container(s_list_screen);
    thistle_ui_set_size(header, -1, 36);
    thistle_ui_set_layout(header, THISTLE_LAYOUT_FLEX_ROW);
    thistle_ui_set_align(header, THISTLE_ALIGN_SPACE_BETWEEN, THISTLE_ALIGN_CENTER);
    label(header, "WiFi Scanner", 18, text);
    thistle_ui_on_event(button(header, "Scan", 70, true), THISTLE_EVENT_CLICK, scan_clicked, 0);

    s_status = label(s_list_screen, "Tap Scan to look for networks", 14, secondary);
    s_channels = label(s_list_screen, "", 14, secondary);

    s_list = thistle_ui_create_container(s_list_screen);
    thistle_ui_set_size(s_list, -1, -1);
    thistle_ui_set_flex_grow(s_list, 1);
    thistle_ui_set_layout(s_list, THISTLE_LAYOUT_FLEX_COLUMN);
    thistle_ui_set_gap(s_list, 4);
    thistle_ui_set_scrollable(s_list, true);

    s_detail_screen = screen();
    thistle_widget_t detail_header = thistle_ui_create_container(s_detail_screen);
    thistle_ui_set_size(detail_header, -1, 36);
    thistle_ui_set_layout(detail_header, THISTLE_LAYOUT_FLEX_ROW);
    thistle_ui_set_gap(detail_header, 8);
    thistle_ui_set_align(detail_header, THISTLE_ALIGN_START, THISTLE_ALIGN_CENTER);
    thistle_ui_on_event(button(detail_header, "Back", 62, false), THISTLE_EVENT_CLICK, back_clicked, 0);
    s_detail_title = label(detail_header, "", 18, text);

    s_detail_info = label(s_detail_screen, "", 14, text);
    s_password_field = thistle_ui_create_text_input(s_detail_screen, "Password");
    thistle_ui_set_size(s_password_field, -1, 36);
    thistle_ui_set_password_mode(s_password_field, true);
    thistle_ui_set_one_line(s_password_field, true);
    s_connect = button(s_detail_screen, "Join", 90, true);
    thistle_ui_on_event(s_connect, THISTLE_EVENT_CLICK, connect_clicked, 0);
    s_detail_status = label(s_detail_screen, "", 14, secondary);
    thistle_ui_set_visible(s_detail_screen, false);
    return 0;
}

static void wifiscanner_on_start(void) { thistle_log(TAG, "on_start"); }
static void wifiscanner_on_pause(void) { thistle_log(TAG, "on_pause"); }
static void wifiscanner_on_resume(void) { thistle_log(TAG, "on_resume"); }

/* Leaving an app destroys it; the next open builds its screens again. */
static void wifiscanner_on_destroy(void)
{
    thistle_log(TAG, "on_destroy");
    thistle_ui_destroy(s_list_screen);
    thistle_ui_destroy(s_detail_screen);
    s_list_screen = s_detail_screen = THISTLE_WIDGET_NONE;
    for (int i = 0; i < MAX_NETWORKS; i++) s_rows[i] = THISTLE_WIDGET_NONE;
    s_count = 0;
    s_selected = -1;
}

static const thistle_app_t wifiscanner_app = {
    .id = "com.thistle.wifiscanner",
    .name = "WiFi Scanner",
    .version = "1.0.0",
    .allow_background = false,
    .on_create = wifiscanner_on_create,
    .on_start = wifiscanner_on_start,
    .on_pause = wifiscanner_on_pause,
    .on_resume = wifiscanner_on_resume,
    .on_destroy = wifiscanner_on_destroy,
};

THISTLE_APP(wifiscanner_app);
