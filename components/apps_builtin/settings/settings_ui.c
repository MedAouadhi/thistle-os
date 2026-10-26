/*
 * settings_ui.c — ThistleOS Settings application UI
 *
 * Navigation: main list -> WiFi
 *                       -> Mobile data (boards with a modem)
 *                       -> Bluetooth, Appearance
 *                       -> Drivers -> per-driver detail
 *                       -> About
 *             Back button returns up one level.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "settings/settings_app.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "thistle/wifi_manager.h"
#include "thistle/net_manager.h"
#include "thistle/ble_manager.h"
#include "thistle/kernel.h"
#include "thistle/signing.h"
#include "thistle/ota.h"
#include "thistle/update.h"
#include "thistle/driver_loader.h"
#include "thistle/light_prefs.h"
#include "thistle/gps_manager.h"
#include "thistle/mesh.h"
#include "thistle/radio_antenna.h"
#include "thistle/refresh_prefs.h"
#include "thistle/power_manager.h"
#include "thistle/time_manager.h"
#include "hal/board.h"
#include "hal/sdcard_path.h"
#include "ui/manager.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/epaper_refresh.h"
#include "ui/statusbar.h"

#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <string.h>
#include <stdint.h>

/* WiFi credential persistence (Rust FFI) */
extern int wifi_manager_save_credentials(const char *ssid, const char *password);

static const char *TAG = "settings_ui";

/* ------------------------------------------------------------------ */
/* Layout constants                                                     */
/* ------------------------------------------------------------------ */

static int s_app_w = 240;
static int s_app_h = 296;
#define TITLE_BAR_H      30
#define ITEM_H           30
#define ITEM_PAD_LEFT     8
#define ITEM_PAD_RIGHT    6
#define SUB_CONTENT_Y    TITLE_BAR_H   /* content starts below title bar */

/* ------------------------------------------------------------------ */
/* Navigation state                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    SETTINGS_MAIN,
    SETTINGS_WIFI,
    SETTINGS_MOBILE,
    SETTINGS_BLUETOOTH,
    SETTINGS_APPEARANCE,
    SETTINGS_POWER,
    SETTINGS_TIME,
    SETTINGS_TIME_ZONE,
    SETTINGS_ABOUT,
    SETTINGS_UPDATE,
    SETTINGS_DRIVERS,
    SETTINGS_DRIVER_DETAIL,
} settings_screen_t;

static settings_screen_t  s_current_screen = SETTINGS_MAIN;
static lv_obj_t          *s_root           = NULL;
static lv_obj_t          *s_main_list      = NULL;
static lv_obj_t          *s_sub_screen     = NULL; /* level-1 sub-screen */
static lv_obj_t          *s_detail_screen  = NULL; /* level-2 driver detail */

/* WiFi init guard — only call wifi_manager_init() once */
static bool s_wifi_inited = false;

/* WiFi sub-screen state */
static lv_obj_t *s_wifi_status_label = NULL;
static lv_obj_t *s_wifi_scan_list    = NULL;

/* Mobile data sub-screen state */
static lv_timer_t *s_cell_timer      = NULL;
static lv_obj_t   *s_cell_status_lbl = NULL;
static lv_obj_t   *s_cell_sim_lbl    = NULL;
static lv_obj_t   *s_cell_net_lbl    = NULL;
static lv_obj_t   *s_cell_signal_lbl = NULL;
static lv_obj_t   *s_cell_ip_lbl     = NULL;
static lv_obj_t   *s_cell_route_lbl  = NULL;
static lv_obj_t   *s_cell_apn_lbl    = NULL;
static lv_obj_t   *s_cell_apn_ta     = NULL;

/* BLE init guard — only call ble_manager_init() once */
static bool s_ble_inited = false;

/* BLE sub-screen state */
static lv_obj_t *s_ble_status_label   = NULL;
static lv_obj_t *s_ble_name_label     = NULL;
static lv_obj_t *s_ble_peer_label     = NULL;
static lv_obj_t *s_ble_pairing_label  = NULL;
static lv_obj_t *s_ble_toggle_btn_lbl = NULL;

/* Power detail live-update timer */
static lv_timer_t *s_power_timer    = NULL;
static lv_obj_t   *s_power_batt_lbl = NULL;
static lv_obj_t   *s_power_pct_lbl  = NULL;
static lv_obj_t   *s_power_state_lbl= NULL;

/* GPS detail live-update timer */
static lv_timer_t *s_gps_timer      = NULL;
static lv_obj_t   *s_gps_status_lbl = NULL;
static lv_obj_t   *s_gps_lat_lbl    = NULL;
static lv_obj_t   *s_gps_lon_lbl    = NULL;
static lv_obj_t   *s_gps_sat_lbl    = NULL;
static lv_obj_t   *s_gps_sig_lbl    = NULL;
static lv_obj_t   *s_gps_orbit_lbl  = NULL;

/* IMU detail live-update timer: one label per axis, accelerometer then gyro */
#define IMU_AXES 6
static lv_timer_t *s_imu_timer = NULL;
static lv_obj_t   *s_imu_lbl[IMU_AXES];

/* Main list dynamic value labels */
static lv_obj_t *s_wifi_value_label = NULL;
static lv_obj_t *s_cell_value_label = NULL;
static lv_obj_t *s_bt_value_label   = NULL;

/* Main list live-update timer */
static lv_timer_t *s_main_timer = NULL;

static void ble_update_labels(void);
static void wifi_update_status_label(void);
static void time_forget_screen(void);
static void update_forget_screen(void);
static void create_note(lv_obj_t *content, const char *text);
static const char *wifi_main_value(void);
static const char *cell_main_value(void);
static void set_label_if_changed(lv_obj_t *label, const char *text);
static void choice_check_only(lv_obj_t *btn);
static lv_obj_t *create_choice_row(lv_obj_t *content);
static void create_choice_button(lv_obj_t *row, const char *text,
                                 lv_event_cb_t cb, uintptr_t value,
                                 bool checked);

/* ------------------------------------------------------------------ */
/* Driver type enum                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    DRIVER_TYPE_DISPLAY,
    DRIVER_TYPE_INPUT,
    DRIVER_TYPE_RADIO,
    DRIVER_TYPE_GPS,
    DRIVER_TYPE_AUDIO,
    DRIVER_TYPE_POWER,
    DRIVER_TYPE_IMU,
    DRIVER_TYPE_STORAGE,
} driver_type_t;

typedef struct {
    driver_type_t type;
    int           index;
} driver_row_data_t;

#define MAX_DRIVER_ROWS 16
static driver_row_data_t s_driver_row_pool[MAX_DRIVER_ROWS];
static int               s_driver_row_pool_used = 0;

/* ------------------------------------------------------------------ */
/* Settings category definitions                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *value; /* NULL = no value shown */
} settings_item_t;

static const settings_item_t s_items[] = {
    { "WiFi",        "Off" },
    { "Mobile data", "Off" },  /* only on a board with a modem */
    { "Bluetooth",   "Off" },
    { "Appearance",  NULL  },
    { "Power",       NULL  },
    { "Date & time", NULL  },
    { "Drivers",     NULL  },
    { "System update", NULL },
    { "About",       NULL  },
};
#define ITEMS_COUNT (sizeof(s_items) / sizeof(s_items[0]))

/* ------------------------------------------------------------------ */
/* Style helpers                                                        */
/* ------------------------------------------------------------------ */

/* Apply the standard BlackBerry monochrome style to a container/panel. */
static void style_panel(lv_obj_t *obj)
{
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_set_style_bg_color(obj, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

/* Apply the title bar style (bg color, 1px bottom border). */
static void style_title_bar(lv_obj_t *obj)
{
    style_panel(obj);
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_set_style_pad_left(obj, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(obj, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, tc->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
}

/* Create a standard row separator (1px horizontal line). */
static lv_obj_t *create_separator(lv_obj_t *parent)
{
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_t *sep = lv_obj_create(parent);
    lv_obj_set_size(sep, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(sep, tc->text, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sep, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(sep, 0, LV_PART_MAIN);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return sep;
}

/* Create an info row: "label: value" with Montserrat 14. */
static lv_obj_t *create_info_row(lv_obj_t *parent, const char *text)
{
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), ITEM_H);
    lv_obj_set_style_bg_color(row, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, tc->text, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    return row;
}

/* Create a plain label row and return the label for live updates. */
static lv_obj_t *create_live_row(lv_obj_t *parent, const char *initial_text)
{
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), ITEM_H);
    lv_obj_set_style_bg_color(row, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, initial_text ? initial_text : "");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, tc->text, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    return lbl; /* caller saves for lv_label_set_text later */
}

/* ------------------------------------------------------------------ */
/* Main list live-update timer                                          */
/* ------------------------------------------------------------------ */

static void settings_main_update_timer(lv_timer_t *t)
{
    (void)t;
    if (s_current_screen == SETTINGS_BLUETOOTH) {
        ble_update_labels();
        return;
    }
    if (s_current_screen == SETTINGS_WIFI) {
        wifi_update_status_label();
        return;
    }
    if (s_current_screen != SETTINGS_MAIN) return;

    if (s_wifi_value_label) {
        set_label_if_changed(s_wifi_value_label, wifi_main_value());
    }
    if (s_cell_value_label) {
        set_label_if_changed(s_cell_value_label, cell_main_value());
    }
    if (s_bt_value_label) {
        ble_state_t bs = ble_manager_get_state();
        const char *bt_val = (bs == BLE_STATE_CONNECTED)   ? "Connected"  :
                             (bs == BLE_STATE_ADVERTISING) ? "Scanning..." : "Off";
        lv_label_set_text(s_bt_value_label, bt_val);
    }
}

/* ------------------------------------------------------------------ */
/* Back navigation                                                      */
/* ------------------------------------------------------------------ */

/* Detail -> Drivers */
static void back_to_drivers(lv_event_t *e)
{
    (void)e;

    /* Destroy live-update timers */
    if (s_power_timer) {
        lv_timer_delete(s_power_timer);
        s_power_timer     = NULL;
        s_power_batt_lbl  = NULL;
        s_power_pct_lbl   = NULL;
        s_power_state_lbl = NULL;
    }
    if (s_gps_timer) {
        lv_timer_delete(s_gps_timer);
        s_gps_timer      = NULL;
        s_gps_status_lbl = NULL;
        s_gps_lat_lbl    = NULL;
        s_gps_lon_lbl    = NULL;
        s_gps_sat_lbl    = NULL;
        s_gps_sig_lbl    = NULL;
        s_gps_orbit_lbl  = NULL;
    }
    if (s_imu_timer) {
        lv_timer_delete(s_imu_timer);
        s_imu_timer = NULL;
        memset(s_imu_lbl, 0, sizeof(s_imu_lbl));
    }

    if (s_detail_screen) {
        lv_obj_delete(s_detail_screen);
        s_detail_screen = NULL;
    }
    if (s_sub_screen) {
        lv_obj_remove_flag(s_sub_screen, LV_OBJ_FLAG_HIDDEN);
    }
    s_current_screen = SETTINGS_DRIVERS;
}

static void cell_forget_screen(void)
{
    if (s_cell_timer) {
        lv_timer_delete(s_cell_timer);
        s_cell_timer = NULL;
    }
    s_cell_status_lbl = NULL;
    s_cell_sim_lbl    = NULL;
    s_cell_net_lbl    = NULL;
    s_cell_signal_lbl = NULL;
    s_cell_ip_lbl     = NULL;
    s_cell_route_lbl  = NULL;
    s_cell_apn_lbl    = NULL;
    s_cell_apn_ta     = NULL;
}

/* Drivers / WiFi / Mobile data / Bluetooth / About -> Main */
static void back_to_main(lv_event_t *e)
{
    (void)e;
    cell_forget_screen();
    time_forget_screen();
    update_forget_screen();
    if (s_sub_screen) {
        lv_obj_delete(s_sub_screen);
        s_sub_screen = NULL;
    }
    s_wifi_status_label    = NULL;
    s_wifi_scan_list       = NULL;
    s_ble_status_label     = NULL;
    s_ble_name_label       = NULL;
    s_ble_peer_label       = NULL;
    s_ble_pairing_label    = NULL;
    s_ble_toggle_btn_lbl   = NULL;
    s_driver_row_pool_used = 0;
    /* Value labels on the main list are still valid after returning */
    lv_obj_remove_flag(s_main_list, LV_OBJ_FLAG_HIDDEN);
    s_current_screen = SETTINGS_MAIN;
}

/* ------------------------------------------------------------------ */
/* Sub-screen scaffold: creates the full-area container + title bar    */
/* Returns the scrollable content area below the title bar.            */
/* ------------------------------------------------------------------ */

/*
 * alloc_sub_screen: generic helper — allocates a full-area container with a
 * title/back bar, parented to container_parent.  Stores the outer container
 * in *out_screen (if non-NULL).  Returns the scrollable content area.
 */
static lv_obj_t *alloc_sub_screen(lv_obj_t *container_parent,
                                  const char *title,
                                  lv_event_cb_t back_cb,
                                  void *back_udata,
                                  lv_obj_t **out_screen)
{
    lv_obj_t *screen = lv_obj_create(container_parent);
    lv_obj_set_pos(screen, 0, 0);
    lv_obj_set_size(screen, s_app_w, s_app_h);
    style_panel(screen);

    if (out_screen) *out_screen = screen;

    const theme_colors_t *tc = theme_get_colors();

    lv_obj_t *title_bar = lv_obj_create(screen);
    lv_obj_set_pos(title_bar, 0, 0);
    lv_obj_set_size(title_bar, s_app_w, TITLE_BAR_H);
    style_title_bar(title_bar);
    lv_obj_add_flag(title_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(title_bar, tc->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(title_bar, back_cb, LV_EVENT_CLICKED, back_udata);
    /* What the Back key presses, with this screen in front. */
    lv_obj_set_user_data(screen, title_bar);

    char back_text[48];
    snprintf(back_text, sizeof(back_text), "< %s", title);
    lv_obj_t *lbl = lv_label_create(title_bar);
    lv_label_set_text(lbl, back_text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, tc->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, tc->bg, LV_STATE_PRESSED);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_set_pos(content, 0, TITLE_BAR_H);
    lv_obj_set_size(content, s_app_w, s_app_h - TITLE_BAR_H);
    lv_obj_set_style_bg_color(content, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(content, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(content, tc->text, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(content, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(content, 0, LV_PART_SCROLLBAR);

    return content;
}

/* Convenience: level-1 sub-screen (back goes to main, stored in s_sub_screen) */
static lv_obj_t *create_sub_screen(const char *title)
{
    return alloc_sub_screen(s_root, title, back_to_main, NULL, &s_sub_screen);
}

/* ------------------------------------------------------------------ */
/* WiFi sub-screen                                                      */
/* ------------------------------------------------------------------ */

/* WiFi's value on the main list. */
static const char *wifi_main_value(void)
{
    if (!wifi_manager_is_enabled()) {
        return "Off";
    }
    switch ((wifi_state_t)wifi_manager_get_state()) {
    case WIFI_STATE_CONNECTED:  return "Connected";
    case WIFI_STATE_CONNECTING: return "Connecting...";
    default:                    return "Not connected";
    }
}

static void wifi_update_status_label(void)
{
    if (!s_wifi_status_label) return;

    if (!wifi_manager_is_enabled()) {
        set_label_if_changed(s_wifi_status_label, "Status: Off");
        return;
    }

    wifi_state_t state = (wifi_state_t)wifi_manager_get_state();
    char buf[64];

    switch (state) {
    case WIFI_STATE_CONNECTED: {
        const char *ip = wifi_manager_get_ip();
        if (ip) {
            snprintf(buf, sizeof(buf), "Status: Connected (%s)", ip);
        } else {
            snprintf(buf, sizeof(buf), "Status: Connected");
        }
        break;
    }
    case WIFI_STATE_CONNECTING:
        snprintf(buf, sizeof(buf), "Status: Connecting...");
        break;
    case WIFI_STATE_FAILED:
        snprintf(buf, sizeof(buf), "Status: Failed");
        break;
    case WIFI_STATE_DISCONNECTED:
    default:
        snprintf(buf, sizeof(buf), "Status: Not connected");
        break;
    }

    set_label_if_changed(s_wifi_status_label, buf);
}

static void wifi_switch_cb(lv_event_t *e)
{
    bool on = (bool)(uintptr_t)lv_event_get_user_data(e);
    esp_err_t err = wifi_manager_set_enabled(on);
    if (wifi_manager_is_enabled() != on) {
        toast_warn(on ? "WiFi did not start" : "WiFi is not available");
        return;
    }
    choice_check_only(lv_event_get_current_target(e));
    if (!on && s_wifi_scan_list) {
        lv_obj_clean(s_wifi_scan_list);
    }
    if (err != ESP_OK) {
        toast_warn("Not saved for next boot");
    }
    wifi_update_status_label();
}

/* ------------------------------------------------------------------ */
/* WiFi password dialog                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char  ssid[WIFI_SSID_MAX_LEN + 1];
    lv_obj_t *dialog;
    lv_obj_t *ta;
} wifi_pwd_ctx_t;

static wifi_pwd_ctx_t s_wifi_pwd_ctx; /* single-instance, screen lifetime */

#define PWD_DIALOG_MAX_W   280
#define PWD_DIALOG_MARGIN    8   /* screen edge to dialog, each side */
#define PWD_DIALOG_BORDER    2
#define PWD_DIALOG_PAD       6
#define PWD_BTN_INSET        4   /* dialog content edge to button, and between */

static void wifi_pwd_connect_cb(lv_event_t *e)
{
    wifi_pwd_ctx_t *ctx = (wifi_pwd_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    /* A copy, because deleting the dialog below frees the text area's own
     * buffer, and the allocator writes its free-list pointers over the
     * start of it. Connecting with that pointer sent the first 8 characters
     * as garbage, so even the right password failed to authenticate. */
    char password[WIFI_PASS_MAX_LEN + 1];
    strncpy(password, lv_textarea_get_text(ctx->ta), sizeof(password) - 1);
    password[sizeof(password) - 1] = '\0';
    ESP_LOGI(TAG, "WiFi: connecting to \"%s\" with password", ctx->ssid);

    if (s_wifi_status_label) {
        lv_label_set_text(s_wifi_status_label, "Status: Connecting...");
    }

    /* Delete dialog before blocking connect call */
    if (ctx->dialog) {
        lv_obj_delete(ctx->dialog);
        ctx->dialog = NULL;
        ctx->ta     = NULL;
    }

    esp_err_t err = wifi_manager_connect(ctx->ssid, password, 10000);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi: connected to \"%s\"", ctx->ssid);
        wifi_manager_save_credentials(ctx->ssid, password);
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "WiFi: connect not supported in simulator");
    } else {
        ESP_LOGW(TAG, "WiFi: connect to \"%s\" failed: 0x%x", ctx->ssid, err);
    }
    memset(password, 0, sizeof(password));
    wifi_update_status_label();
}

/* The dialog goes with its screen, too, when that is left with it open. */
static void wifi_pwd_deleted_cb(lv_event_t *e)
{
    wifi_pwd_ctx_t *ctx = (wifi_pwd_ctx_t *)lv_event_get_user_data(e);
    ctx->dialog = NULL;
    ctx->ta     = NULL;
}

static void wifi_pwd_cancel_cb(lv_event_t *e)
{
    wifi_pwd_ctx_t *ctx = (wifi_pwd_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->dialog) {
        lv_obj_delete(ctx->dialog);
        ctx->dialog = NULL;
        ctx->ta     = NULL;
    }
}

static void show_wifi_password_dialog(const char *ssid)
{
    const theme_colors_t *tc = theme_get_colors();

    /* Fill context */
    strncpy(s_wifi_pwd_ctx.ssid, ssid, sizeof(s_wifi_pwd_ctx.ssid) - 1);
    s_wifi_pwd_ctx.ssid[sizeof(s_wifi_pwd_ctx.ssid) - 1] = '\0';

    /* Overlay on sub_screen, as wide as the screen allows less a margin each
     * side. A fixed 280 px hung 20 px off both edges of a 240 px panel: the
     * first letter of every line and the outer borders of both buttons were
     * off screen. Everything inside is sized from this width. */
    int32_t dialog_w = s_app_w - 2 * PWD_DIALOG_MARGIN;
    if (dialog_w > PWD_DIALOG_MAX_W) dialog_w = PWD_DIALOG_MAX_W;
    int32_t content_w = dialog_w - 2 * (PWD_DIALOG_BORDER + PWD_DIALOG_PAD);
    int32_t btn_w = (content_w - 3 * PWD_BTN_INSET) / 2;

    lv_obj_t *dialog = lv_obj_create(s_sub_screen);
    lv_obj_set_size(dialog, dialog_w, 140);
    lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dialog, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dialog, PWD_DIALOG_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dialog, tc->text, LV_PART_MAIN);
    lv_obj_set_style_radius(dialog, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dialog, PWD_DIALOG_PAD, LV_PART_MAIN);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(dialog);

    s_wifi_pwd_ctx.dialog = dialog;
    lv_obj_add_event_cb(dialog, wifi_pwd_deleted_cb, LV_EVENT_DELETE, &s_wifi_pwd_ctx);

    /* SSID title — one line, shortened with dots if the name is too long */
    lv_obj_t *title = lv_label_create(dialog);
    lv_label_set_text_fmt(title, "Connect to: %s", ssid);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, tc->text, LV_PART_MAIN);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(title, content_w, lv_font_get_line_height(&lv_font_montserrat_14));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    /* Password textarea */
    lv_obj_t *ta = lv_textarea_create(dialog);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_placeholder_text(ta, "Password");
    lv_obj_set_width(ta, content_w);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, tc->text, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, tc->surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, tc->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(ta, 1, LV_PART_MAIN);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 26);

    s_wifi_pwd_ctx.ta = ta;

    /* Cancel button */
    lv_obj_t *btn_cancel = lv_obj_create(dialog);
    lv_obj_set_size(btn_cancel, btn_w, 28);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, PWD_BTN_INSET, -4);
    lv_obj_set_style_bg_color(btn_cancel, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn_cancel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_cancel, tc->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_cancel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_cancel, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_cancel, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_cancel, tc->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_cancel, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(btn_cancel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn_cancel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_cancel, wifi_pwd_cancel_cb, LV_EVENT_CLICKED, &s_wifi_pwd_ctx);

    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_set_style_text_font(lbl_cancel, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_cancel, tc->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_cancel, tc->bg, LV_STATE_PRESSED);
    lv_obj_align(lbl_cancel, LV_ALIGN_CENTER, 0, 0);

    /* Connect button */
    lv_obj_t *btn_connect = lv_obj_create(dialog);
    lv_obj_set_size(btn_connect, btn_w, 28);
    lv_obj_align(btn_connect, LV_ALIGN_BOTTOM_RIGHT, -PWD_BTN_INSET, -4);
    lv_obj_set_style_bg_color(btn_connect, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn_connect, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_connect, tc->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_connect, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_connect, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_connect, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_connect, tc->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_connect, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(btn_connect, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn_connect, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_connect, wifi_pwd_connect_cb, LV_EVENT_CLICKED, &s_wifi_pwd_ctx);

    lv_obj_t *lbl_connect = lv_label_create(btn_connect);
    lv_label_set_text(lbl_connect, "Connect");
    lv_obj_set_style_text_font(lbl_connect, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_connect, tc->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_connect, tc->bg, LV_STATE_PRESSED);
    lv_obj_align(lbl_connect, LV_ALIGN_CENTER, 0, 0);
}

/* Called when a scan result network row is clicked. */
static void wifi_network_clicked_cb(lv_event_t *e)
{
    wifi_scan_result_t *net = (wifi_scan_result_t *)lv_event_get_user_data(e);
    if (!net) return;

    ESP_LOGI(TAG, "WiFi: attempting connect to \"%s\" (open=%d)", net->ssid, net->is_open);

    if (!net->is_open) {
        /* Secured network — show password dialog */
        show_wifi_password_dialog(net->ssid);
        return;
    }

    /* Open network — connect directly */
    if (s_wifi_status_label) {
        lv_label_set_text(s_wifi_status_label, "Status: Connecting...");
    }

    esp_err_t err = wifi_manager_connect(net->ssid, NULL, 10000);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi: connected to \"%s\"", net->ssid);
        wifi_manager_save_credentials(net->ssid, NULL);
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "WiFi: connect not supported in simulator");
    } else {
        ESP_LOGW(TAG, "WiFi: connect to \"%s\" failed: 0x%x", net->ssid, err);
    }

    wifi_update_status_label();
}

/* Scan results buffer — heap-allocated on demand to save DRAM. */
static wifi_scan_result_t *s_scan_results = NULL;
static uint8_t             s_scan_count = 0;

static void wifi_scan_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (!wifi_manager_is_enabled()) {
        toast_info("Switch WiFi on to scan");
        return;
    }
    ESP_LOGI(TAG, "WiFi: scanning...");

    if (s_wifi_status_label) {
        lv_label_set_text(s_wifi_status_label, "Status: Scanning...");
    }

    /* Clear previous scan list entries */
    if (s_wifi_scan_list) {
        lv_obj_clean(s_wifi_scan_list);
    }

    s_scan_count = 0;
    if (!s_scan_results) {
        s_scan_results = malloc(sizeof(wifi_scan_result_t) * WIFI_SCAN_MAX_RESULTS);
        if (!s_scan_results) {
            ESP_LOGE(TAG, "WiFi: scan alloc failed");
            return;
        }
    }
    esp_err_t err = wifi_manager_scan(s_scan_results, WIFI_SCAN_MAX_RESULTS, &s_scan_count);

    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "WiFi: scan not supported in simulator");
        /* Show a placeholder entry in the list */
        if (s_wifi_scan_list) {
            create_info_row(s_wifi_scan_list, "(Scan not available in simulator)");
        }
        wifi_update_status_label();
        return;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi: scan failed: 0x%x", err);
        /* Busy rejoining the saved network in the background. */
        toast_warn(err == ESP_ERR_TIMEOUT ? "WiFi is busy; scan again shortly"
                                          : "Scan failed");
        wifi_update_status_label();
        return;
    }

    ESP_LOGI(TAG, "WiFi: found %d networks", s_scan_count);
    wifi_update_status_label();

    if (!s_wifi_scan_list) return;

    const theme_colors_t *tc_scan = theme_get_colors();
    for (uint8_t i = 0; i < s_scan_count; i++) {
        wifi_scan_result_t *net = &s_scan_results[i];

        /* Row */
        lv_obj_t *row = lv_obj_create(s_wifi_scan_list);
        lv_obj_set_size(row, LV_PCT(100), ITEM_H);
        lv_obj_set_style_bg_color(row, tc_scan->bg, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_left(row, ITEM_PAD_LEFT, LV_PART_MAIN);
        lv_obj_set_style_pad_right(row, ITEM_PAD_RIGHT, LV_PART_MAIN);
        lv_obj_set_style_pad_top(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(row, 0, LV_PART_MAIN);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, tc_scan->text, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, tc_scan->primary, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row,
                              LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, wifi_network_clicked_cb, LV_EVENT_CLICKED, net);

        /* SSID label */
        lv_obj_t *lbl_ssid = lv_label_create(row);
        char ssid_text[WIFI_SSID_MAX_LEN + 8];
        if (!net->is_open) {
            snprintf(ssid_text, sizeof(ssid_text), "%s [*]", net->ssid);
        } else {
            snprintf(ssid_text, sizeof(ssid_text), "%s", net->ssid);
        }
        lv_label_set_text(lbl_ssid, ssid_text);
        lv_obj_set_style_text_font(lbl_ssid, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl_ssid, tc_scan->text, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl_ssid, tc_scan->bg, LV_STATE_PRESSED);
        lv_obj_set_flex_grow(lbl_ssid, 1);
        /* One line, shortened with dots: wrapped, a long name spilled out of
         * the bottom of its row into the next. */
        lv_label_set_long_mode(lbl_ssid, LV_LABEL_LONG_DOT);
        lv_obj_set_height(lbl_ssid, lv_font_get_line_height(&lv_font_montserrat_14));

        /* RSSI label */
        lv_obj_t *lbl_rssi = lv_label_create(row);
        char rssi_text[16];
        snprintf(rssi_text, sizeof(rssi_text), "%d dBm", net->rssi);
        lv_label_set_text(lbl_rssi, rssi_text);
        lv_obj_set_style_text_font(lbl_rssi, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl_rssi, tc_scan->text, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl_rssi, tc_scan->bg, LV_STATE_PRESSED);
    }
}

static void open_wifi_screen(void)
{
    /* Initialize WiFi subsystem once */
    if (!s_wifi_inited) {
        esp_err_t err = wifi_manager_init();
        if (err == ESP_OK) {
            s_wifi_inited = true;
        } else {
            ESP_LOGW(TAG, "wifi_manager_init failed: 0x%x", err);
        }
    }

    lv_obj_t *content = create_sub_screen("WiFi");
    s_current_screen = SETTINGS_WIFI;

    const theme_colors_t *tc_wifi = theme_get_colors();

    /* Off stops the radio; the saved network is kept for when it is on. */
    bool on = wifi_manager_is_enabled();
    lv_obj_t *switch_row = create_choice_row(content);
    create_choice_button(switch_row, "On", wifi_switch_cb, 1, on);
    create_choice_button(switch_row, "Off", wifi_switch_cb, 0, !on);
    create_separator(content);

    /* Status row */
    lv_obj_t *status_row = lv_obj_create(content);
    lv_obj_set_size(status_row, LV_PCT(100), ITEM_H);
    lv_obj_set_style_bg_color(status_row, tc_wifi->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(status_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(status_row, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(status_row, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(status_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(status_row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_wifi_status_label = lv_label_create(status_row);
    lv_label_set_text(s_wifi_status_label, "Status: Disconnected");
    lv_obj_set_style_text_font(s_wifi_status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wifi_status_label, tc_wifi->text, LV_PART_MAIN);
    lv_obj_align(s_wifi_status_label, LV_ALIGN_LEFT_MID, 0, 0);
    wifi_update_status_label();

    create_separator(content);

    /* Scan Networks button */
    lv_obj_t *btn = lv_obj_create(content);
    lv_obj_set_size(btn, LV_PCT(100), ITEM_H);
    lv_obj_set_style_bg_color(btn, tc_wifi->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(btn, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(btn, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, tc_wifi->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_FULL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, tc_wifi->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, wifi_scan_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_btn = lv_label_create(btn);
    lv_label_set_text(lbl_btn, "Scan Networks");
    lv_obj_set_style_text_font(lbl_btn, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_btn, tc_wifi->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_btn, tc_wifi->bg, LV_STATE_PRESSED);
    lv_obj_align(lbl_btn, LV_ALIGN_CENTER, 0, 0);

    create_separator(content);

    /* Scan results list (initially empty, populated by scan button) */
    s_wifi_scan_list = lv_obj_create(content);
    lv_obj_set_width(s_wifi_scan_list, LV_PCT(100));
    lv_obj_set_height(s_wifi_scan_list, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_wifi_scan_list, tc_wifi->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_wifi_scan_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_wifi_scan_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_wifi_scan_list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_wifi_scan_list, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_wifi_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_wifi_scan_list,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(s_wifi_scan_list, LV_OBJ_FLAG_SCROLLABLE);
}

/* ------------------------------------------------------------------ */
/* Mobile data sub-screen                                               */
/* ------------------------------------------------------------------ */

/* The e-paper redraws whatever is set, changed or not. */
static void set_label_if_changed(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static const char *cell_sim_text(hal_sim_state_t sim)
{
    switch (sim) {
    case HAL_SIM_READY:        return "Ready";
    case HAL_SIM_ABSENT:       return "Not inserted";
    case HAL_SIM_PIN_REQUIRED: return "Locked by its PIN";
    case HAL_SIM_PUK_REQUIRED: return "Blocked (needs PUK)";
    case HAL_SIM_ERROR:        return "Not usable";
    default:                   return "--";
    }
}

static const char *cell_reg_text(hal_cell_reg_t reg)
{
    switch (reg) {
    case HAL_CELL_REG_HOME:           return "Registered";
    case HAL_CELL_REG_ROAMING:        return "Roaming";
    case HAL_CELL_REG_SEARCHING:      return "Searching...";
    case HAL_CELL_REG_DENIED:         return "Refused";
    case HAL_CELL_REG_NOT_REGISTERED: return "Not registered";
    default:                          return "--";
    }
}

/* Why mobile data is not carrying data, or that it is. */
static const char *cell_status_text(const net_cellular_status_t *st)
{
    if (!st->enabled && !st->sms) {
        return "Off";
    }
    if (st->sim_blocked) {
        switch (st->info.sim) {
        case HAL_SIM_ABSENT:       return "No SIM card";
        case HAL_SIM_PIN_REQUIRED: return "SIM has a PIN";
        case HAL_SIM_PUK_REQUIRED: return "SIM blocked";
        default:                   return "SIM not usable";
        }
    }
    if (!st->enabled) {
        /* The modem is kept registered for calls and SMS alone. */
        return st->info.powered && st->info.sim == HAL_SIM_READY ? "Calls & SMS"
                                                                 : "Starting";
    }
    switch (st->data) {
    case HAL_NET_STATE_CONNECTED:  return "Connected";
    case HAL_NET_STATE_CONNECTING: return "Connecting...";
    /* An attempt failed, or the session dropped: the coordinator tries
     * again, after 10 s and up to 2 min. */
    default:                       return "Waiting to retry";
    }
}

static const char *cell_main_value(void)
{
    net_cellular_status_t st;
    if (net_cellular_get_status(&st) != ESP_OK || !st.present) {
        return "--";
    }
    return cell_status_text(&st);
}

static void cell_update_labels(void)
{
    net_cellular_status_t st;
    if (net_cellular_get_status(&st) != ESP_OK) {
        return;
    }
    char buf[64];

    snprintf(buf, sizeof(buf), "Status: %s", cell_status_text(&st));
    set_label_if_changed(s_cell_status_lbl, buf);

    bool live = (st.enabled || st.sms) && (st.info.powered || st.sim_blocked);
    snprintf(buf, sizeof(buf), "SIM: %s", live ? cell_sim_text(st.info.sim) : "--");
    set_label_if_changed(s_cell_sim_lbl, buf);

    if (live && st.info.operator_name[0]) {
        snprintf(buf, sizeof(buf), "Network: %.23s%s", st.info.operator_name,
                 st.info.reg == HAL_CELL_REG_ROAMING ? " (roaming)" : "");
    } else {
        snprintf(buf, sizeof(buf), "Network: %s",
                 live ? cell_reg_text(st.info.reg) : "--");
    }
    set_label_if_changed(s_cell_net_lbl, buf);

    /* Read before the data session began: a modem carrying data over PPP
     * answers no questions. */
    if (live && st.info.rssi_dbm != 0) {
        snprintf(buf, sizeof(buf), "Signal: %d dBm", (int)st.info.rssi_dbm);
    } else {
        snprintf(buf, sizeof(buf), "Signal: --");
    }
    set_label_if_changed(s_cell_signal_lbl, buf);

    const hal_net_driver_t *active = net_get_active_underlay();
    const char *ip = NULL;
    if (st.data == HAL_NET_STATE_CONNECTED) {
        const hal_net_driver_t *list[4];
        int n = net_list_transports(list, 4);
        for (int i = 0; i < n; i++) {
            if (list[i]->type == HAL_NET_CELLULAR && list[i]->get_ip) {
                ip = list[i]->get_ip();
            }
        }
    }
    snprintf(buf, sizeof(buf), "IP: %s", ip && ip[0] ? ip : "--");
    set_label_if_changed(s_cell_ip_lbl, buf);

    /* Which link traffic takes: WiFi whenever it is connected. */
    snprintf(buf, sizeof(buf), "In use: %s",
             active && active->name ? active->name : "nothing");
    set_label_if_changed(s_cell_route_lbl, buf);
}

static void cell_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_current_screen == SETTINGS_MOBILE) {
        cell_update_labels();
    }
}

static void cell_switch_cb(lv_event_t *e)
{
    bool on = (bool)(uintptr_t)lv_event_get_user_data(e);
    esp_err_t err = net_cellular_set_enabled(on);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        toast_warn("No modem on this board");
        return;
    }
    choice_check_only(lv_event_get_current_target(e));
    if (err != ESP_OK) {
        toast_warn("Not saved for next boot");
    } else if (on) {
        /* Starting the modem and finding a network takes a while. */
        toast_info("Starting the modem");
    }
    cell_update_labels();
    net_cellular_status_t st;
    if (on && net_cellular_get_status(&st) == ESP_OK && !st.sim_blocked
        && st.data == HAL_NET_STATE_DISCONNECTED) {
        /* The coordinator has yet to start the attempt. */
        set_label_if_changed(s_cell_status_lbl, "Status: Connecting...");
    }
}

static void cell_sms_switch_cb(lv_event_t *e)
{
    bool on = (bool)(uintptr_t)lv_event_get_user_data(e);
    esp_err_t err = net_cellular_set_sms(on);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        toast_warn("No modem on this board");
        return;
    }
    choice_check_only(lv_event_get_current_target(e));
    if (err != ESP_OK) {
        toast_warn("Not saved for next boot");
    }
    cell_update_labels();
}

static void cell_show_apn(void)
{
    if (!s_cell_apn_lbl) return;
    char apn[64] = "";
    net_cellular_get_apn(apn, sizeof(apn));
    char buf[80];
    snprintf(buf, sizeof(buf), "APN: %s", apn[0] ? apn : "automatic");
    set_label_if_changed(s_cell_apn_lbl, buf);
}

static void cell_apn_set_cb(lv_event_t *e)
{
    (void)e;
    if (!s_cell_apn_ta) return;

    esp_err_t err = net_cellular_set_apn(lv_textarea_get_text(s_cell_apn_ta));
    if (err == ESP_ERR_INVALID_ARG) {
        toast_warn("Letters, digits, . - _ only");
        return;
    }
    if (err != ESP_OK) {
        toast_warn("APN not saved");
        return;
    }
    lv_textarea_set_text(s_cell_apn_ta, "");
    cell_show_apn();
    net_cellular_status_t st;
    if (net_cellular_get_status(&st) == ESP_OK && st.enabled) {
        toast_info("APN saved; reconnecting");
    } else {
        toast_info("APN saved");
    }
}

/* A row whose label is cut with "..." rather than run off the screen. */
static lv_obj_t *create_cell_row(lv_obj_t *content)
{
    lv_obj_t *lbl = create_live_row(content, "");
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    return lbl;
}

static void open_mobile_screen(void)
{
    lv_obj_t *content = create_sub_screen("Mobile data");
    s_current_screen = SETTINGS_MOBILE;

    net_cellular_status_t st;
    bool have = net_cellular_get_status(&st) == ESP_OK;
    bool enabled = have && st.enabled;
    bool sms = have && st.sms;

    lv_obj_t *row = create_choice_row(content);
    create_choice_button(row, "On", cell_switch_cb, 1, enabled);
    create_choice_button(row, "Off", cell_switch_cb, 0, !enabled);

    /* With mobile data off, this keeps the modem registered, without data,
     * so calls and SMS still arrive. */
    create_info_row(content, "Calls and SMS");
    row = create_choice_row(content);
    create_choice_button(row, "On", cell_sms_switch_cb, 1, sms);
    create_choice_button(row, "Off", cell_sms_switch_cb, 0, !sms);
    create_separator(content);

    s_cell_status_lbl = create_cell_row(content);
    s_cell_sim_lbl    = create_cell_row(content);
    s_cell_net_lbl    = create_cell_row(content);
    s_cell_signal_lbl = create_cell_row(content);
    s_cell_ip_lbl     = create_cell_row(content);
    create_separator(content);
    s_cell_route_lbl  = create_cell_row(content);
    create_separator(content);

    /* Most networks hand the modem an APN; some SIMs need theirs typed. */
    s_cell_apn_lbl = create_cell_row(content);
    cell_show_apn();
    lv_obj_t *apn_row = create_choice_row(content);
    s_cell_apn_ta = lv_textarea_create(apn_row);
    lv_textarea_set_one_line(s_cell_apn_ta, true);
    lv_textarea_set_max_length(s_cell_apn_ta, 63);
    lv_textarea_set_placeholder_text(s_cell_apn_ta, "APN, empty = auto");
    lv_obj_set_flex_grow(s_cell_apn_ta, 1);
    lv_obj_set_height(s_cell_apn_ta, ITEM_H - 4);
    lv_obj_set_style_text_font(s_cell_apn_ta, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_cell_apn_ta, 2, LV_PART_MAIN);
    create_choice_button(apn_row, "Set", cell_apn_set_cb, 0, false);

    cell_update_labels();
    s_cell_timer = lv_timer_create(cell_timer_cb, 2000, NULL);
}

/* ------------------------------------------------------------------ */
/* Bluetooth sub-screen                                                 */
/* ------------------------------------------------------------------ */

static void ble_update_labels(void)
{
    if (!s_ble_status_label) return;

    ble_state_t state = ble_manager_get_state();
    const char *state_str;
    switch (state) {
    case BLE_STATE_ADVERTISING: state_str = "Status: Advertising"; break;
    case BLE_STATE_CONNECTED:   state_str = "Status: Connected";   break;
    default:                    state_str = "Status: Off";         break;
    }
    lv_label_set_text(s_ble_status_label, state_str);

    if (s_ble_name_label) {
        lv_label_set_text(s_ble_name_label, "Name: ThistleOS");
    }

    if (s_ble_peer_label) {
        const char *peer = ble_manager_get_peer_name();
        char peer_buf[64];
        snprintf(peer_buf, sizeof(peer_buf), "Peer: %s", peer ? peer : "None");
        lv_label_set_text(s_ble_peer_label, peer_buf);
        lv_obj_set_style_opa(s_ble_peer_label,
                             (state == BLE_STATE_CONNECTED) ? LV_OPA_COVER : LV_OPA_40,
                             LV_PART_MAIN);
    }

    if (s_ble_pairing_label) {
        uint32_t passkey = ble_manager_get_pairing_passkey();
        char pairing_buf[40];
        if (passkey != 0) {
            snprintf(pairing_buf, sizeof(pairing_buf), "Pairing code: %06" PRIu32, passkey);
        } else {
            snprintf(pairing_buf, sizeof(pairing_buf), "Pairing code: waiting");
        }
        lv_label_set_text(s_ble_pairing_label, pairing_buf);
    }

    if (s_ble_toggle_btn_lbl) {
        lv_label_set_text(s_ble_toggle_btn_lbl,
                          (state == BLE_STATE_OFF) ? "Enable" : "Disable");
    }
}

static void ble_toggle_clicked_cb(lv_event_t *e)
{
    (void)e;
    ble_state_t state = ble_manager_get_state();
    if (state == BLE_STATE_OFF) {
        if (!s_ble_inited) {
            esp_err_t err = ble_manager_init("ThistleOS");
            if (err == ESP_OK) {
                s_ble_inited = true;
            } else {
                ESP_LOGW(TAG, "ble_manager_init failed: 0x%x", err);
                return;
            }
        }
        ble_manager_start_advertising();
    } else if (state == BLE_STATE_CONNECTED) {
        ble_manager_disconnect();
    } else {
        ble_manager_stop_advertising();
    }
    ble_update_labels();
}

static void ble_disconnect_clicked_cb(lv_event_t *e)
{
    (void)e;
    ble_manager_disconnect();
    ble_update_labels();
}

static void open_bluetooth_screen(void)
{
    lv_obj_t *content = create_sub_screen("Bluetooth");
    s_current_screen = SETTINGS_BLUETOOTH;

    const theme_colors_t *tc_bt = theme_get_colors();

    /* Status row */
    lv_obj_t *status_row = lv_obj_create(content);
    lv_obj_set_size(status_row, LV_PCT(100), ITEM_H);
    lv_obj_set_style_bg_color(status_row, tc_bt->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(status_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(status_row, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(status_row, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(status_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(status_row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_ble_status_label = lv_label_create(status_row);
    lv_label_set_text(s_ble_status_label, "Status: Off");
    lv_obj_set_style_text_font(s_ble_status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ble_status_label, tc_bt->text, LV_PART_MAIN);
    lv_obj_align(s_ble_status_label, LV_ALIGN_LEFT_MID, 0, 0);

    create_separator(content);

    /* Device name row */
    lv_obj_t *name_row = create_info_row(content, "Name: ThistleOS");
    s_ble_name_label = lv_obj_get_child(name_row, 0);

    create_separator(content);

    /* Peer name row (shown dim when not connected) */
    lv_obj_t *peer_row = create_info_row(content, "Peer: None");
    s_ble_peer_label = lv_obj_get_child(peer_row, 0);
    lv_obj_set_style_opa(s_ble_peer_label, LV_OPA_40, LV_PART_MAIN);

    create_separator(content);

    /* Secure Connections passkey, populated when a peer starts pairing. */
    lv_obj_t *pairing_row = create_info_row(content, "Pairing code: waiting");
    s_ble_pairing_label = lv_obj_get_child(pairing_row, 0);

    create_separator(content);

    /* Enable / Disable toggle button */
    lv_obj_t *toggle_btn = lv_obj_create(content);
    lv_obj_set_size(toggle_btn, LV_PCT(100), ITEM_H);
    lv_obj_set_style_bg_color(toggle_btn, tc_bt->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(toggle_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(toggle_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(toggle_btn, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(toggle_btn, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(toggle_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(toggle_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(toggle_btn, tc_bt->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(toggle_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(toggle_btn, LV_BORDER_SIDE_FULL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(toggle_btn, tc_bt->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(toggle_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(toggle_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(toggle_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(toggle_btn, ble_toggle_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_ble_toggle_btn_lbl = lv_label_create(toggle_btn);
    lv_label_set_text(s_ble_toggle_btn_lbl, "Enable");
    lv_obj_set_style_text_font(s_ble_toggle_btn_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ble_toggle_btn_lbl, tc_bt->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ble_toggle_btn_lbl, tc_bt->bg, LV_STATE_PRESSED);
    lv_obj_align(s_ble_toggle_btn_lbl, LV_ALIGN_CENTER, 0, 0);

    create_separator(content);

    /* Disconnect button (dimmed when not connected) */
    lv_obj_t *disc_btn = lv_obj_create(content);
    lv_obj_set_size(disc_btn, LV_PCT(100), ITEM_H);
    lv_obj_set_style_bg_color(disc_btn, tc_bt->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(disc_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(disc_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(disc_btn, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(disc_btn, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(disc_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(disc_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(disc_btn, tc_bt->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(disc_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(disc_btn, LV_BORDER_SIDE_FULL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(disc_btn, tc_bt->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(disc_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_opa(disc_btn,
                         (ble_manager_get_state() == BLE_STATE_CONNECTED)
                             ? LV_OPA_COVER : LV_OPA_40,
                         LV_PART_MAIN);
    lv_obj_add_flag(disc_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(disc_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(disc_btn, ble_disconnect_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_disc = lv_label_create(disc_btn);
    lv_label_set_text(lbl_disc, "Disconnect");
    lv_obj_set_style_text_font(lbl_disc, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_disc, tc_bt->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_disc, tc_bt->bg, LV_STATE_PRESSED);
    lv_obj_align(lbl_disc, LV_ALIGN_CENTER, 0, 0);

    ble_update_labels();
}

/* ------------------------------------------------------------------ */
/* Appearance sub-screen                                                */
/* ------------------------------------------------------------------ */

#define APPEARANCE_MAX_THEMES 8

/* Payload type for theme selection rows */
typedef struct {
    char name[32]; /* theme filename, e.g. "dark.json" */
} appearance_theme_payload_t;

static appearance_theme_payload_t *s_theme_payloads = NULL; /* heap-allocated to save DRAM */

static void theme_selected_cb(lv_event_t *e)
{
    const char *theme_name = (const char *)lv_event_get_user_data(e);
    if (!theme_name) return;

    if (strcmp(theme_name, "__default__") == 0) {
        /* Load the built-in default monochrome theme from SD if available,
         * otherwise reinitialise the default without a display argument.
         * theme_init(NULL) skips display wiring but re-applies styles. */
        char def_path[128];
        snprintf(def_path, sizeof(def_path), "%s/themes/default.json", THISTLE_SDCARD);
        esp_err_t ret = theme_load(def_path);
        if (ret != ESP_OK) {
            theme_init(NULL);
            statusbar_refresh_theme();
        }
        toast_info("Default theme applied");
    } else {
        char path[128];
        snprintf(path, sizeof(path), "%s/themes/%s", THISTLE_SDCARD, theme_name);
        esp_err_t ret = theme_load(path);
        if (ret == ESP_OK) {
            toast_info("Theme applied");
        } else {
            toast_warn("Failed to load theme");
        }
    }
}

static void wallpaper_browse_cb(lv_event_t *e)
{
    (void)e;
    /* File browser not yet implemented */
    toast_info("File browser: coming soon");
}

static void wallpaper_clear_cb(lv_event_t *e)
{
    (void)e;
    /* Nothing to clear yet — placeholder */
    toast_info("Wallpaper cleared");
}

/* Create a clickable theme-selection row showing [x] or [ ] indicator */
static void create_theme_row(lv_obj_t *content, const char *display_name,
                             const char *payload_name, bool is_active)
{
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_t *row = lv_obj_create(content);
    lv_obj_set_size(row, LV_PCT(100), ITEM_H);
    lv_obj_set_style_bg_color(row, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, ITEM_PAD_LEFT + 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, tc->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, tc->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, theme_selected_cb, LV_EVENT_CLICKED, (void *)payload_name);

    lv_obj_t *lbl_name = lv_label_create(row);
    lv_label_set_text(lbl_name, display_name);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_name, tc->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_name, tc->bg, LV_STATE_PRESSED);
    lv_obj_set_flex_grow(lbl_name, 1);

    lv_obj_t *lbl_sel = lv_label_create(row);
    lv_label_set_text(lbl_sel, is_active ? "[x]" : "[ ]");
    lv_obj_set_style_text_font(lbl_sel, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_sel, tc->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_sel, tc->bg, LV_STATE_PRESSED);
}

/* Create a clickable action button row (for Browse / Clear wallpaper) */
static lv_obj_t *create_action_row(lv_obj_t *content, const char *label,
                                   lv_event_cb_t cb)
{
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_t *btn = lv_obj_create(content);
    lv_obj_set_size(btn, LV_PCT(100), ITEM_H);
    lv_obj_set_style_bg_color(btn, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(btn, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(btn, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, tc->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_FULL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, tc->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, tc->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, tc->bg, LV_STATE_PRESSED);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    return btn;
}

static void open_appearance_screen(void)
{
    lv_obj_t *content = create_sub_screen("Appearance");
    s_current_screen = SETTINGS_APPEARANCE;

    /* ---- Theme section header ---- */
    create_info_row(content, "Theme");
    create_separator(content);

    /* Use theme_get_current_name() to determine active theme.
     * Default name is "Default"; SD themes are stored as e.g. "dark.json". */
    const char *current_name = theme_get_current_name();
    bool default_active = (strcmp(current_name, "Default") == 0);

    /* Allocate theme payloads on first use */
    if (!s_theme_payloads) {
        s_theme_payloads = malloc(sizeof(appearance_theme_payload_t) * (APPEARANCE_MAX_THEMES + 1));
        if (!s_theme_payloads) return;
        memset(s_theme_payloads, 0, sizeof(appearance_theme_payload_t) * (APPEARANCE_MAX_THEMES + 1));
    }

    /* Store "__default__" in the first payload slot */
    strncpy(s_theme_payloads[0].name, "__default__", sizeof(s_theme_payloads[0].name) - 1);
    create_theme_row(content, "Default", s_theme_payloads[0].name, default_active);

    /* Enumerate themes from /sdcard/themes */
    char theme_names[APPEARANCE_MAX_THEMES][32];
    int theme_count = theme_list_available(theme_names, APPEARANCE_MAX_THEMES);

    for (int i = 0; i < theme_count; i++) {
        /* Build display name: strip ".json" suffix */
        char display[32];
        strncpy(display, theme_names[i], sizeof(display) - 1);
        display[sizeof(display) - 1] = '\0';
        size_t dlen = strlen(display);
        if (dlen > 5 && strcmp(display + dlen - 5, ".json") == 0) {
            display[dlen - 5] = '\0';
        }

        /* Copy filename into persistent payload storage */
        int slot = i + 1; /* slot 0 is Default */
        if (slot >= APPEARANCE_MAX_THEMES + 1) break;
        strncpy(s_theme_payloads[slot].name, theme_names[i],
                sizeof(s_theme_payloads[slot].name) - 1);
        s_theme_payloads[slot].name[sizeof(s_theme_payloads[slot].name) - 1] = '\0';

        /* Mark active if this filename matches the currently loaded theme */
        bool active = (strcmp(current_name, theme_names[i]) == 0);

        create_theme_row(content, display, s_theme_payloads[slot].name, active);
    }

    if (theme_count == 0) {
        create_info_row(content, "  (no themes on SD card)");
    }

    create_separator(content);

    /* ---- Wallpaper section header ---- */
    create_info_row(content, "Wallpaper");
    create_separator(content);

    create_info_row(content, "  (current: none)");
    create_separator(content);

    create_action_row(content, "Browse SD Card", wallpaper_browse_cb);
    create_separator(content);
    create_action_row(content, "Clear Wallpaper", wallpaper_clear_cb);
}

/* ------------------------------------------------------------------ */
/* About sub-screen                                                     */
/* ------------------------------------------------------------------ */

static void open_about_screen(void)
{
    lv_obj_t *content = create_sub_screen("About");
    s_current_screen = SETTINGS_ABOUT;

    const theme_colors_t *tc_about = theme_get_colors();

    /* ThistleOS title */
    lv_obj_t *title_row = lv_obj_create(content);
    lv_obj_set_size(title_row, LV_PCT(100), ITEM_H);
    lv_obj_set_style_bg_color(title_row, tc_about->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(title_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(title_row, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(title_row, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(title_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(title_row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(title_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl_os = lv_label_create(title_row);
    lv_label_set_text(lbl_os, "ThistleOS");
    lv_obj_set_style_text_font(lbl_os, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_os, tc_about->text, LV_PART_MAIN);
    lv_obj_align(lbl_os, LV_ALIGN_LEFT_MID, 0, 0);

    /* Version */
    char ver_buf[64];
    snprintf(ver_buf, sizeof(ver_buf), "Version: %s", ota_get_current_version());
    create_info_row(content, ver_buf);

    create_separator(content);

    /* Board name */
    const hal_registry_t *reg = hal_get_registry();
    char board_buf[64];
    snprintf(board_buf, sizeof(board_buf), "Board: %s",
             (reg && reg->board_name) ? reg->board_name : "Unknown");
    create_info_row(content, board_buf);

    create_separator(content);

    /* Display driver */
    char disp_buf[64];
    if (reg && reg->display && reg->display->name) {
        snprintf(disp_buf, sizeof(disp_buf), "Display: %s %ux%u",
                 reg->display->name,
                 reg->display->width,
                 reg->display->height);
    } else {
        snprintf(disp_buf, sizeof(disp_buf), "Display: Unknown");
    }
    create_info_row(content, disp_buf);

    /* Radio driver */
    char radio_buf[48];
    if (reg && reg->radio && reg->radio->name) {
        snprintf(radio_buf, sizeof(radio_buf), "Radio: %s", reg->radio->name);
    } else {
        snprintf(radio_buf, sizeof(radio_buf), "Radio: None");
    }
    create_info_row(content, radio_buf);

    /* GPS driver */
    char gps_buf[48];
    if (reg && reg->gps && reg->gps->name) {
        snprintf(gps_buf, sizeof(gps_buf), "GPS: %s", reg->gps->name);
    } else {
        snprintf(gps_buf, sizeof(gps_buf), "GPS: None");
    }
    create_info_row(content, gps_buf);

    create_separator(content);

    /* Free heap */
    char heap_buf[48];
    snprintf(heap_buf, sizeof(heap_buf), "Free heap: %lu bytes",
             (unsigned long)esp_get_free_heap_size());
    create_info_row(content, heap_buf);

    /* PSRAM free */
    char psram_buf[48];
    snprintf(psram_buf, sizeof(psram_buf), "PSRAM free: %lu bytes",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    create_info_row(content, psram_buf);

    /* Uptime */
    uint32_t uptime_ms = kernel_uptime_ms();
    uint32_t s  = uptime_ms / 1000;
    uint32_t h  = s / 3600;
    uint32_t m  = (s % 3600) / 60;
    uint32_t sc = s % 60;
    char uptime_buf[48];
    snprintf(uptime_buf, sizeof(uptime_buf), "Uptime: %02lu:%02lu:%02lu",
             (unsigned long)h, (unsigned long)m, (unsigned long)sc);
    create_info_row(content, uptime_buf);

    create_separator(content);

    /* Signing key (truncated) */
    const char *key_hex = signing_get_public_key_hex();
    char sign_buf[48];
    snprintf(sign_buf, sizeof(sign_buf), "Signing key: %.16s...", key_hex);
    create_info_row(content, sign_buf);

    /* Runtime drivers loaded from SD card */
    char drv_buf[64];
    int rt_drv_count = driver_loader_get_count();
    if (rt_drv_count > 0) {
        snprintf(drv_buf, sizeof(drv_buf), "Runtime drivers: %d loaded from SD", rt_drv_count);
    } else {
        snprintf(drv_buf, sizeof(drv_buf), "Runtime drivers: none");
    }
    create_info_row(content, drv_buf);
}

/* ------------------------------------------------------------------ */
/* System update sub-screen                                             */
/* ------------------------------------------------------------------ */

/* A release copied to the card's update/ folder installs from here, once its
 * artifact manifest holds up (update_manager.rs); anything else is refused
 * with the reason shown. The install runs on a task of the kernel's; this
 * screen polls rs_update_get_status(). Go back boots the version still in
 * the other kernel slot, while rs_update_go_back_version() offers it. */

static lv_timer_t *s_update_timer      = NULL;
static lv_obj_t   *s_update_card_lbl   = NULL;
static lv_obj_t   *s_update_status_lbl = NULL;
static lv_obj_t   *s_update_install    = NULL;
static lv_obj_t   *s_update_restart    = NULL;
static lv_obj_t   *s_update_later      = NULL;
static lv_obj_t   *s_update_go_back    = NULL;
/* The version Go back reaches, as last shown; "" while it is not offered. */
static char        s_update_back_shown[40];
/* Later hides Restart now until the next install. */
static bool        s_update_later_chosen = false;

static void update_forget_screen(void)
{
    if (s_update_timer) {
        lv_timer_delete(s_update_timer);
        s_update_timer = NULL;
    }
    s_update_card_lbl   = NULL;
    s_update_status_lbl = NULL;
    s_update_install    = NULL;
    s_update_restart    = NULL;
    s_update_later      = NULL;
    s_update_go_back    = NULL;
    s_update_back_shown[0] = '\0';
}

static void update_set_shown(lv_obj_t *obj, bool shown)
{
    if (!obj) return;
    bool hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (shown && hidden) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (!shown && !hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void update_refresh(lv_timer_t *t)
{
    (void)t;
    update_status_t st;
    rs_update_get_status(&st);

    char card[64];
    char version[40] = "";
    bool on_card = rs_update_card_release(version, sizeof(version));
    if (!on_card) {
        snprintf(card, sizeof(card), "No release on the card");
    } else if (version[0]) {
        snprintf(card, sizeof(card), "On the card: %s", version);
    } else {
        snprintf(card, sizeof(card), "On the card: a release");
    }
    if (s_update_card_lbl && strcmp(lv_label_get_text(s_update_card_lbl), card) != 0) {
        lv_label_set_text(s_update_card_lbl, card);
        ESP_LOGI(TAG, "System update: %s", card);
    }

    char status[200];
    switch (st.state) {
    case UPDATE_WRITING:
        if (st.total > 0) {
            snprintf(status, sizeof(status), "Installing %s: %lu%%", st.version,
                     (unsigned long)((uint64_t)st.done * 100 / st.total));
        } else {
            snprintf(status, sizeof(status), "Checking the release...");
        }
        break;
    case UPDATE_READY:
        snprintf(status, sizeof(status), "%s is installed. It runs after a restart.", st.version);
        break;
    case UPDATE_FAILED:
        snprintf(status, sizeof(status), "Not installed: %s", st.reason);
        break;
    default:
        status[0] = '\0';
        break;
    }
    if (s_update_status_lbl && strcmp(lv_label_get_text(s_update_status_lbl), status) != 0) {
        lv_label_set_text(s_update_status_lbl, status);
        if (status[0]) ESP_LOGI(TAG, "System update: %s", status);
    }

    bool busy = st.state == UPDATE_WRITING;
    bool ready = st.state == UPDATE_READY;
    update_set_shown(s_update_install, on_card && !busy && !ready);
    update_set_shown(s_update_restart, ready && !s_update_later_chosen);
    update_set_shown(s_update_later, ready && !s_update_later_chosen);

    /* Go back, while the other kernel slot still holds the previous version
     * (rs_update_go_back_version decides). */
    char back[40] = "";
    if (!rs_update_go_back_version(back, sizeof(back))) back[0] = '\0';
    if (s_update_go_back && strcmp(back, s_update_back_shown) != 0) {
        if (back[0]) {
            char text[64];
            snprintf(text, sizeof(text), "Go back to %s", back);
            lv_label_set_text(lv_obj_get_child(s_update_go_back, 0), text);
            ESP_LOGI(TAG, "System update: %s", text);
        } else if (s_update_back_shown[0]) {
            ESP_LOGI(TAG, "System update: going back to %s is no longer offered",
                     s_update_back_shown);
        }
        snprintf(s_update_back_shown, sizeof(s_update_back_shown), "%s", back);
    }
    /* Hidden while an install runs: what it does to the slot shows after. */
    update_set_shown(s_update_go_back, back[0] != '\0' && !busy);
}

static void update_install_cb(lv_event_t *e)
{
    (void)e;
    s_update_later_chosen = false;
    if (rs_update_install_from_card() != ESP_OK) {
        toast_warn("An update is already installing");
    }
    update_refresh(NULL);
}

static void update_restart_cb(lv_event_t *e)
{
    (void)e;
    rs_update_restart();
}

static void update_later_cb(lv_event_t *e)
{
    (void)e;
    s_update_later_chosen = true;
    toast_show("It runs after the next restart", TOAST_INFO, 3000);
    update_refresh(NULL);
}

static void update_go_back_cb(lv_event_t *e)
{
    (void)e;
    /* Restarts into the other kernel slot; returns only if it cannot. */
    if (rs_update_go_back() != ESP_OK) {
        toast_warn("Cannot go back to the previous version");
    }
    update_refresh(NULL);
}

static void open_update_screen(void)
{
    lv_obj_t *content = create_sub_screen("System update");
    s_current_screen = SETTINGS_UPDATE;

    char running[64];
    snprintf(running, sizeof(running), "Running: %s", rs_update_running_version());
    /* Two lines, whatever the version: a laptop build's is long, and the
     * rows below stay where they are. */
    lv_obj_t *running_lbl = create_live_row(content, running);
    lv_obj_set_height(lv_obj_get_parent(running_lbl), 2 * ITEM_H - 8);
    lv_label_set_long_mode(running_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(running_lbl, LV_PCT(100));
    lv_obj_align(running_lbl, LV_ALIGN_TOP_LEFT, 0, 6);
    ESP_LOGI(TAG, "System update: %s", running);

    s_update_card_lbl = create_live_row(content, "");
    s_update_install  = create_action_row(content, "Install from card", update_install_cb);

    const theme_colors_t *tc = theme_get_colors();
    s_update_status_lbl = lv_label_create(content);
    lv_label_set_text(s_update_status_lbl, "");
    lv_label_set_long_mode(s_update_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_update_status_lbl, LV_PCT(100));
    lv_obj_set_style_pad_left(s_update_status_lbl, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_update_status_lbl, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_update_status_lbl, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_update_status_lbl, 4, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_update_status_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_update_status_lbl, tc->text, LV_PART_MAIN);

    s_update_restart = create_action_row(content, "Restart now", update_restart_cb);
    s_update_later   = create_action_row(content, "Later", update_later_cb);
    s_update_go_back = create_action_row(content, "Go back", update_go_back_cb);
    /* A laptop build's version is longer than the row. */
    lv_obj_t *back_lbl = lv_obj_get_child(s_update_go_back, 0);
    lv_obj_set_width(back_lbl, LV_PCT(100));
    lv_label_set_long_mode(back_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(back_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    s_update_back_shown[0] = '\0';
    lv_obj_add_flag(s_update_go_back, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_update_install, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_update_restart, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_update_later, LV_OBJ_FLAG_HIDDEN);

    create_note(content, "To install a release from the card, copy its image, "
                         "manifest and signature to the card's update folder.");

    update_refresh(NULL);
    s_update_timer = lv_timer_create(update_refresh, 500, NULL);
}

/* ------------------------------------------------------------------ */
/* Driver detail screens                                                */
/* ------------------------------------------------------------------ */

/* An Off/Low/Med/Full row for a light — the screen's or the keys'. Each
 * button hands its level to cb as a percent in the event's user data.
 * offer_off is false for an emissive screen: with its light out it shows
 * nothing, not even the button that would bring it back. */
static void create_light_level_row(lv_obj_t *content, lv_event_cb_t cb,
                                   bool offer_off)
{
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_t *btn_row = lv_obj_create(content);
    lv_obj_set_size(btn_row, LV_PCT(100), ITEM_H + 4);
    lv_obj_set_style_bg_color(btn_row, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(btn_row, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(btn_row, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btn_row, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(btn_row, 2, LV_PART_MAIN);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 6, LV_PART_MAIN);

    static const struct {
        const char *label;
        uint8_t     percent;
    } levels[4] = {
        { "Off",  0   },
        { "Low",  25  },
        { "Med",  60  },
        { "Full", 100 },
    };

    for (int i = offer_off ? 0 : 1; i < 4; i++) {
        lv_obj_t *b = lv_obj_create(btn_row);
        lv_obj_set_size(b, LV_SIZE_CONTENT, ITEM_H - 4);
        lv_obj_set_style_bg_color(b, tc->bg, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(b, tc->text, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_FULL, LV_PART_MAIN);
        lv_obj_set_style_radius(b, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_left(b, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_right(b, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_top(b, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(b, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, tc->primary, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)levels[i].percent);

        lv_obj_t *bl = lv_label_create(b);
        lv_label_set_text(bl, levels[i].label);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(bl, tc->text, LV_PART_MAIN);
        lv_obj_set_style_text_color(bl, tc->bg, LV_STATE_PRESSED);
        lv_obj_align(bl, LV_ALIGN_CENTER, 0, 0);
    }
    create_separator(content);
}

/* --- Display --- */

static void display_brightness_cb(lv_event_t *e)
{
    uint8_t percent = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    esp_err_t ret = light_set_display(percent, false);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Display: brightness -> %u%%", (unsigned)percent);
        /* The light is already set; a save that fails costs only the level
         * at the next boot, which is what the user should hear about. */
        if (light_prefs_save_display(percent) != ESP_OK) {
            toast_warn("Brightness not saved for next boot");
        }
    } else if (ret == ESP_ERR_NOT_SUPPORTED) {
        /* E-paper with no front-light wired — the T-Deck Pro's. */
        toast_warn("No screen light on this board");
    } else {
        toast_warn("Failed to set brightness");
    }
}

/* --- Choice rows: one button per option, the option in use inverted --- */

/* Mark a choice button as the option in use, or not. Its label carries the
 * state too: the theme colours labels itself, so a label does not take the
 * button's colour and would stay black on the inverted button. */
static void choice_set_checked(lv_obj_t *btn, bool checked)
{
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (checked) {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
        if (label) lv_obj_add_state(label, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(btn, LV_STATE_CHECKED);
        if (label) lv_obj_remove_state(label, LV_STATE_CHECKED);
    }
}

/* Mark the tapped button as the option in use, and only it. */
static void choice_check_only(lv_obj_t *btn)
{
    lv_obj_t *row = lv_obj_get_parent(btn);
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); i++) {
        lv_obj_t *other = lv_obj_get_child(row, (int32_t)i);
        choice_set_checked(other, other == btn);
    }
}

static lv_obj_t *create_choice_row(lv_obj_t *content)
{
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_t *btn_row = lv_obj_create(content);
    lv_obj_set_size(btn_row, LV_PCT(100), ITEM_H + 4);
    lv_obj_set_style_bg_color(btn_row, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(btn_row, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(btn_row, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btn_row, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(btn_row, 2, LV_PART_MAIN);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 6, LV_PART_MAIN);
    return btn_row;
}

static void create_choice_button(lv_obj_t *row, const char *text,
                                 lv_event_cb_t cb, uintptr_t value,
                                 bool checked)
{
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_t *b = lv_obj_create(row);
    lv_obj_set_size(b, LV_SIZE_CONTENT, ITEM_H - 4);
    lv_obj_set_style_bg_color(b, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, tc->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_FULL, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(b, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_right(b, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_top(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(b, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, tc->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    /* The option in use reads inverted. */
    lv_obj_set_style_bg_color(b, tc->text, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)value);

    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, text);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(bl, tc->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(bl, tc->bg, LV_STATE_CHECKED);
    lv_obj_align(bl, LV_ALIGN_CENTER, 0, 0);

    choice_set_checked(b, checked);
}

static void display_refresh_mode_cb(lv_event_t *e)
{
    epaper_refresh_policy_t policy =
        (epaper_refresh_policy_t)(uintptr_t)lv_event_get_user_data(e);
    choice_check_only(lv_event_get_current_target(e));

    /* The UI manager applies the policy to every panel refresh, starting with
     * the one this tap's redraw brings, so Full shows itself at once. */
    epaper_refresh_set_policy(policy);
    ESP_LOGI(TAG, "Display: refresh policy -> %d", (int)policy);
    if (refresh_prefs_save((uint8_t)policy) != ESP_OK) {
        toast_warn("Refresh mode not saved for next boot");
    }
}

static void open_detail_display(lv_obj_t *content)
{
    const hal_registry_t *reg = hal_get_registry();
    if (!reg || !reg->display) {
        create_info_row(content, "No display registered");
        return;
    }
    const hal_display_driver_t *d = reg->display;
    char buf[64];

    snprintf(buf, sizeof(buf), "Name: %s", d->name ? d->name : "Unknown");
    create_info_row(content, buf);
    create_separator(content);

    snprintf(buf, sizeof(buf), "Type: %s",
             d->type == HAL_DISPLAY_TYPE_EPAPER ? "E-Paper" : "LCD");
    create_info_row(content, buf);
    create_separator(content);

    snprintf(buf, sizeof(buf), "Resolution: %u x %u", d->width, d->height);
    create_info_row(content, buf);
    create_separator(content);

    /* Only e-paper has refresh modes. An LCD's driver takes the call and
     * ignores it, which made this row three buttons that did nothing. */
    if (d->type == HAL_DISPLAY_TYPE_EPAPER && d->set_refresh_mode) {
        create_info_row(content, "Refresh mode:");
        create_separator(content);

        /* Full is clean but flashes on every update; Fast is quick but leaves
         * ghosting; Auto is Fast with a full refresh after every few. */
        static const struct {
            const char *label;
            epaper_refresh_policy_t policy;
        } modes[3] = {
            { "Full", EPAPER_REFRESH_POLICY_FULL },
            { "Auto", EPAPER_REFRESH_POLICY_AUTO },
            { "Fast", EPAPER_REFRESH_POLICY_FAST },
        };
        epaper_refresh_policy_t current = epaper_refresh_get_policy();

        lv_obj_t *btn_row = create_choice_row(content);
        for (int i = 0; i < 3; i++) {
            create_choice_button(btn_row, modes[i].label, display_refresh_mode_cb,
                                 (uintptr_t)modes[i].policy,
                                 modes[i].policy == current);
        }
        create_separator(content);
    }

    /* As with the key backlight, the vtable entry is all there is to go on: a
     * panel with no light wired answers NOT_SUPPORTED from the buttons. */
    if (d->set_brightness) {
        create_info_row(content,
                        d->type == HAL_DISPLAY_TYPE_EPAPER ? "Front light:"
                                                           : "Brightness:");
        create_separator(content);
        create_light_level_row(content, display_brightness_cb,
                               d->type == HAL_DISPLAY_TYPE_EPAPER);
    }
}

/* --- Radio --- */

static void radio_antenna_cb(lv_event_t *e)
{
    radio_antenna_t antenna = (radio_antenna_t)(uintptr_t)lv_event_get_user_data(e);

    /* The mark moves only if the switch did: a row showing External while the
     * radio is still on the internal antenna is worse than no row at all. */
    if (radio_antenna_select(antenna) != ESP_OK) {
        toast_warn("Could not switch antenna");
        return;
    }
    choice_check_only(lv_event_get_current_target(e));
    ESP_LOGI(TAG, "Radio: antenna -> %s",
             antenna == RADIO_ANTENNA_EXTERNAL ? "external" : "internal");

    if (radio_antenna_save(antenna) != ESP_OK) {
        toast_warn("Antenna not saved for next boot");
    } else if (antenna == RADIO_ANTENNA_EXTERNAL) {
        /* Nothing can tell whether it is there, and sending into an empty
         * connector reflects the power back into the radio. */
        toast_warn("Screw the antenna on before sending");
    }
}

/* --- MeshCore node --- */

static lv_obj_t *s_mesh_name_ta;
static lv_obj_t *s_mesh_name_lbl;

static void mesh_rename_cb(lv_event_t *e)
{
    (void)e;
    if (!s_mesh_name_ta) return;

    char name[32];
    strncpy(name, lv_textarea_get_text(s_mesh_name_ta), sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    esp_err_t err = rs_mesh_set_name(name);
    if (err == ESP_ERR_INVALID_ARG) {
        toast_warn("Enter a name first");
        return;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        toast_warn("The mesh is not running");
        return;
    }
    if (s_mesh_name_lbl) {
        lv_label_set_text_fmt(s_mesh_name_lbl, "Name: %s", rs_mesh_get_self_name());
    }
    lv_textarea_set_text(s_mesh_name_ta, "");
    if (err == ESP_OK) {
        toast_info("Renamed, and advertised");
    } else {
        /* Renamed on air, but system.json would not take it. */
        toast_warn("Name not saved for next boot");
    }
}

/* What another MeshCore node needs to hear this one: the same radio settings,
 * and then an advert to add it by. */
static void open_mesh_rows(lv_obj_t *content)
{
    s_mesh_name_ta = NULL;
    s_mesh_name_lbl = NULL;

    if (!rs_mesh_is_running()) {
        create_info_row(content, "MeshCore: off");
        return;
    }
    char buf[64];

    create_info_row(content, "MeshCore: on air");
    create_separator(content);

    snprintf(buf, sizeof(buf), "Name: %s", rs_mesh_get_self_name());
    s_mesh_name_lbl = create_live_row(content, buf);
    create_separator(content);

    uint8_t key[32];
    if (rs_mesh_get_self_key(key) == ESP_OK) {
        snprintf(buf, sizeof(buf), "Key: %02x%02x%02x%02x%02x%02x...",
                 key[0], key[1], key[2], key[3], key[4], key[5]);
        create_info_row(content, buf);
        create_separator(content);
    }

    mesh_radio_t radio;
    if (rs_mesh_get_radio(&radio) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%" PRIu32 ".%03" PRIu32 " MHz, BW %" PRIu32 ".%" PRIu32 " kHz",
                 radio.freq_hz / 1000000, (radio.freq_hz / 1000) % 1000,
                 radio.bw_hz / 1000, (radio.bw_hz % 1000) / 100);
        create_info_row(content, buf);
        snprintf(buf, sizeof(buf), "SF%u, CR 4/%u, %d dBm",
                 (unsigned)radio.sf, (unsigned)radio.cr, (int)radio.tx_power_dbm);
        create_info_row(content, buf);
        create_separator(content);
    }

    snprintf(buf, sizeof(buf), "Contacts: %d", rs_mesh_get_contact_count());
    create_info_row(content, buf);
    create_separator(content);

    /* A new name goes out in an advert straight away. */
    lv_obj_t *row = create_choice_row(content);
    s_mesh_name_ta = lv_textarea_create(row);
    lv_textarea_set_one_line(s_mesh_name_ta, true);
    lv_textarea_set_max_length(s_mesh_name_ta, 31);
    lv_textarea_set_placeholder_text(s_mesh_name_ta, "New name");
    lv_obj_set_flex_grow(s_mesh_name_ta, 1);
    lv_obj_set_height(s_mesh_name_ta, ITEM_H - 4);
    lv_obj_set_style_text_font(s_mesh_name_ta, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_mesh_name_ta, 2, LV_PART_MAIN);
    create_choice_button(row, "Rename", mesh_rename_cb, 0, false);
    create_separator(content);
}

static void open_detail_radio(lv_obj_t *content)
{
    const hal_registry_t *reg = hal_get_registry();
    if (!reg || !reg->radio) {
        create_info_row(content, "No radio registered");
        return;
    }
    const hal_radio_driver_t *r = reg->radio;
    char buf[64];

    snprintf(buf, sizeof(buf), "Name: %s", r->name ? r->name : "Unknown");
    create_info_row(content, buf);
    create_separator(content);

    create_info_row(content, "Protocol: LoRa");
    create_separator(content);

    if (r->get_rssi) {
        int rssi = r->get_rssi();
        snprintf(buf, sizeof(buf), "RSSI: %d dBm", rssi);
        create_info_row(content, buf);
        create_separator(content);
    }

    /* Only a board whose profile declares an RF switch has a choice: the
     * T-Deck Max's top connector, or the antenna inside the case. */
    if (radio_antenna_switchable()) {
        create_info_row(content, "Antenna:");
        create_separator(content);

        uint8_t current = radio_antenna_get();
        lv_obj_t *row = create_choice_row(content);
        create_choice_button(row, "Internal", radio_antenna_cb,
                             RADIO_ANTENNA_INTERNAL,
                             current == RADIO_ANTENNA_INTERNAL);
        create_choice_button(row, "External", radio_antenna_cb,
                             RADIO_ANTENNA_EXTERNAL,
                             current == RADIO_ANTENNA_EXTERNAL);
        create_separator(content);
    }

    open_mesh_rows(content);
}

/* --- GPS (with live timer) --- */

static void gps_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    const hal_registry_t *reg = hal_get_registry();
    if (!reg || !reg->gps || !s_gps_status_lbl) return;

    hal_gps_position_t pos;
    esp_err_t err = reg->gps->get_position(&pos);
    char buf[48];

    /* Whatever the receiver is doing: the orbits are fetched whether or not
     * it is being read. */
    rs_gps_assist_describe(buf, sizeof(buf));
    lv_label_set_text(s_gps_orbit_lbl, buf);

    if (err != ESP_OK) {
        lv_label_set_text(s_gps_status_lbl, rs_gps_is_enabled() ? "Status: Error" : "Status: Off");
        lv_label_set_text(s_gps_lat_lbl,    "Lat: --");
        lv_label_set_text(s_gps_lon_lbl,    "Lon: --");
        lv_label_set_text(s_gps_sat_lbl,    "Satellites: --");
        lv_label_set_text(s_gps_sig_lbl,    "Signal: --");
        return;
    }

    if (pos.fix_valid) {
        lv_label_set_text(s_gps_status_lbl, "Status: Fix");
        snprintf(buf, sizeof(buf), "Lat: %.6f", pos.latitude);
        lv_label_set_text(s_gps_lat_lbl, buf);
        snprintf(buf, sizeof(buf), "Lon: %.6f", pos.longitude);
        lv_label_set_text(s_gps_lon_lbl, buf);
    } else {
        lv_label_set_text(s_gps_status_lbl, "Status: No fix");
        lv_label_set_text(s_gps_lat_lbl,    "Lat: --");
        lv_label_set_text(s_gps_lon_lbl,    "Lon: --");
    }

    /* What it hears says more than the satellites in use, which stay at 0
     * until the fix whether it needs a better view or hears nothing. */
    hal_gps_sky_t sky;
    if (reg->gps->get_sky && reg->gps->get_sky(&sky) == ESP_OK) {
        snprintf(buf, sizeof(buf), "Satellites: %u used, %u in view",
                 pos.satellites, sky.in_view);
        lv_label_set_text(s_gps_sat_lbl, buf);
        if (sky.heard) {
            snprintf(buf, sizeof(buf), "Signal: %u heard, best %u dB-Hz",
                     sky.heard, sky.best_cno);
        } else {
            snprintf(buf, sizeof(buf), "Signal: none heard");
        }
        lv_label_set_text(s_gps_sig_lbl, buf);
    } else {
        snprintf(buf, sizeof(buf), "Satellites: %u", pos.satellites);
        lv_label_set_text(s_gps_sat_lbl, buf);
        lv_label_set_text(s_gps_sig_lbl, "Signal: --");
    }
}

static void gps_toggle_cb(lv_event_t *e)
{
    lv_obj_t *btn_lbl = (lv_obj_t *)lv_event_get_user_data(e);
    bool on = !rs_gps_is_enabled();
    esp_err_t err = rs_gps_set_enabled(on);
    if (rs_gps_is_enabled() != on) {
        toast_warn(on ? "GPS did not start" : "GPS did not stop");
        return;
    }
    if (btn_lbl) lv_label_set_text(btn_lbl, on ? "Disable" : "Enable");
    if (err != ESP_OK) {
        toast_warn("Not saved for next boot");
    }
}

static void open_detail_gps(lv_obj_t *content)
{
    const hal_registry_t *reg = hal_get_registry();
    if (!reg || !reg->gps) {
        create_info_row(content, "No GPS registered");
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "Name: %s",
             reg->gps->name ? reg->gps->name : "Unknown");
    create_info_row(content, buf);
    create_separator(content);

    s_gps_status_lbl = create_live_row(content, "Status: ...");
    create_separator(content);
    s_gps_lat_lbl    = create_live_row(content, "Lat: --");
    create_separator(content);
    s_gps_lon_lbl    = create_live_row(content, "Lon: --");
    create_separator(content);
    s_gps_sat_lbl    = create_live_row(content, "Satellites: --");
    create_separator(content);
    s_gps_sig_lbl    = create_live_row(content, "Signal: --");
    create_separator(content);
    s_gps_orbit_lbl  = create_live_row(content, "Orbits: --");
    create_separator(content);

    /* Enable / Disable button */
    const theme_colors_t *tc_gps = theme_get_colors();
    lv_obj_t *btn = lv_obj_create(content);
    lv_obj_set_size(btn, LV_PCT(100), ITEM_H);
    lv_obj_set_style_bg_color(btn, tc_gps->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(btn, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(btn, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, tc_gps->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_FULL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, tc_gps->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, rs_gps_is_enabled() ? "Disable" : "Enable");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(bl, tc_gps->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(bl, tc_gps->bg, LV_STATE_PRESSED);
    lv_obj_align(bl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn, gps_toggle_cb, LV_EVENT_CLICKED, bl);

    /* Live update every 2 s */
    s_gps_timer = lv_timer_create(gps_timer_cb, 2000, NULL);
    lv_timer_ready(s_gps_timer);
}

/* --- Power (live timer, 5 s) --- */

static void power_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    const hal_registry_t *reg = hal_get_registry();
    if (!reg || !reg->power || !s_power_batt_lbl) return;

    hal_power_info_t info = {0};
    char buf[48];

    if (reg->power->get_info) {
        if (reg->power->get_info(&info) != ESP_OK) {
            lv_label_set_text(s_power_batt_lbl,  "Battery: Error");
            lv_label_set_text(s_power_pct_lbl,   "Charge: --");
            lv_label_set_text(s_power_state_lbl, "State: Error");
            return;
        }
    } else {
        if (reg->power->get_battery_mv)      info.voltage_mv = reg->power->get_battery_mv();
        if (reg->power->get_battery_percent) info.percent    = reg->power->get_battery_percent();
        if (reg->power->is_charging) {
            info.state = reg->power->is_charging()
                         ? HAL_POWER_STATE_CHARGING
                         : HAL_POWER_STATE_DISCHARGING;
        }
    }

    snprintf(buf, sizeof(buf), "Battery: %u mV", info.voltage_mv);
    lv_label_set_text(s_power_batt_lbl, buf);

    snprintf(buf, sizeof(buf), "Charge: %u%%", info.percent);
    lv_label_set_text(s_power_pct_lbl, buf);

    const char *state_str;
    switch (info.state) {
    case HAL_POWER_STATE_CHARGING:   state_str = "Charging";    break;
    case HAL_POWER_STATE_CHARGED:    state_str = "Charged";     break;
    case HAL_POWER_STATE_NO_BATTERY: state_str = "No Battery";  break;
    default:                         state_str = "Discharging"; break;
    }
    snprintf(buf, sizeof(buf), "State: %s", state_str);
    lv_label_set_text(s_power_state_lbl, buf);
}

static void open_detail_power(lv_obj_t *content)
{
    const hal_registry_t *reg = hal_get_registry();
    if (!reg || !reg->power) {
        create_info_row(content, "No power driver registered");
        return;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "Name: %s",
             reg->power->name ? reg->power->name : "Unknown");
    create_info_row(content, buf);
    create_separator(content);

    s_power_batt_lbl  = create_live_row(content, "Battery: ...");
    create_separator(content);
    s_power_pct_lbl   = create_live_row(content, "Charge: ...");
    create_separator(content);
    s_power_state_lbl = create_live_row(content, "State: ...");

    /* Live update every 5 s, fire immediately */
    s_power_timer = lv_timer_create(power_timer_cb, 5000, NULL);
    lv_timer_ready(s_power_timer);
}

/* --- Audio --- */

static void open_detail_audio(lv_obj_t *content)
{
    const hal_registry_t *reg = hal_get_registry();
    if (!reg || !reg->audio) {
        create_info_row(content, "No audio driver registered");
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "Name: %s",
             reg->audio->name ? reg->audio->name : "Unknown");
    create_info_row(content, buf);
    create_separator(content);

    create_info_row(content, "Volume: 100%");
    create_separator(content);
    create_info_row(content, "(Volume control: future work)");
}

/* --- Storage --- */

static void open_detail_storage(lv_obj_t *content, int index)
{
    const hal_registry_t *reg = hal_get_registry();
    if (!reg || index < 0 || index >= (int)reg->storage_count
            || !reg->storage[index]) {
        create_info_row(content, "No storage driver registered");
        return;
    }
    const hal_storage_driver_t *st = reg->storage[index];
    char buf[64];

    snprintf(buf, sizeof(buf), "Name: %s", st->name ? st->name : "Unknown");
    create_info_row(content, buf);
    create_separator(content);

    snprintf(buf, sizeof(buf), "Type: %s",
             st->type == HAL_STORAGE_TYPE_SD ? "SD Card" : "Internal");
    create_info_row(content, buf);
    create_separator(content);

    bool mounted = st->is_mounted ? st->is_mounted() : false;
    snprintf(buf, sizeof(buf), "Mounted: %s", mounted ? "Yes" : "No");
    create_info_row(content, buf);
    create_separator(content);

    if (mounted && st->get_total_bytes && st->get_free_bytes) {
        uint64_t total  = st->get_total_bytes();
        uint64_t free_b = st->get_free_bytes();
        uint64_t used   = (total > free_b) ? (total - free_b) : 0;

#define FMT_BYTES(v, out_buf) \
    do { \
        if ((v) >= (1024ULL * 1024 * 1024)) { \
            snprintf((out_buf), sizeof(out_buf), "%.1f GB", \
                     (double)(v) / (1024.0 * 1024.0 * 1024.0)); \
        } else { \
            snprintf((out_buf), sizeof(out_buf), "%.1f MB", \
                     (double)(v) / (1024.0 * 1024.0)); \
        } \
    } while (0)

        char sz[24];
        FMT_BYTES(total, sz);
        snprintf(buf, sizeof(buf), "Total: %s", sz);
        create_info_row(content, buf);
        create_separator(content);

        FMT_BYTES(free_b, sz);
        snprintf(buf, sizeof(buf), "Free: %s", sz);
        create_info_row(content, buf);
        create_separator(content);

        uint8_t used_pct = (total > 0) ? (uint8_t)((used * 100ULL) / total) : 0;
        FMT_BYTES(used, sz);
        snprintf(buf, sizeof(buf), "Used: %s (%u%%)", sz, used_pct);
        create_info_row(content, buf);

#undef FMT_BYTES
    } else if (!mounted) {
        create_info_row(content, "(Not mounted)");
    }
}

/* --- Input --- */

static void input_backlight_cb(lv_event_t *e)
{
    uint8_t percent = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    /* Whichever input driver has a backlight gets the level. A driver whose
     * board left the backlight pin unwired reports NOT_SUPPORTED. Saying so
     * beats a button that quietly does nothing. */
    if (light_set_keys(percent, false) != ESP_OK) {
        toast_warn("No key backlight on this board");
        return;
    }
    ESP_LOGI(TAG, "Input: key backlight -> %u%%", (unsigned)percent);
    if (light_prefs_save_keys(percent) != ESP_OK) {
        toast_warn("Key backlight not saved for next boot");
    }
}

static void open_detail_input(lv_obj_t *content, int index)
{
    const hal_registry_t *reg = hal_get_registry();
    if (!reg || index < 0 || index >= (int)reg->input_count
            || !reg->inputs[index]) {
        create_info_row(content, "No input driver registered");
        return;
    }
    const hal_input_driver_t *inp = reg->inputs[index];
    char buf[64];

    snprintf(buf, sizeof(buf), "Name: %s", inp->name ? inp->name : "Unknown");
    create_info_row(content, buf);
    create_separator(content);

    snprintf(buf, sizeof(buf), "Type: %s", inp->is_touch ? "Touch" : "Keyboard");
    create_info_row(content, buf);
    create_separator(content);

    /* A keyboard whose keys can be lit offers a level to set them to. The
     * vtable entry is the only thing to go on here: asking the driver whether
     * a backlight is wired means calling the setter, which would change the
     * level just to find out. A board that left the pin unwired returns
     * NOT_SUPPORTED from the buttons below — the same contract the display's
     * brightness has. */
    if (!inp->set_backlight) {
        create_info_row(content, "(Info only - no settings)");
        return;
    }

    create_info_row(content, "Key backlight:");
    create_separator(content);
    create_light_level_row(content, input_backlight_cb, true);
}

/* --- IMU (live timer, 2 s) --- */

static const char *const IMU_AXIS_NAMES[IMU_AXES] = {
    "Accel X", "Accel Y", "Accel Z", "Gyro X", "Gyro Y", "Gyro Z",
};

static void imu_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    const hal_registry_t *reg = hal_get_registry();
    if (!reg || !reg->imu || !reg->imu->get_data || !s_imu_lbl[0]) return;

    hal_imu_data_t data = {0};
    bool ok = reg->imu->get_data(&data) == ESP_OK;
    const float values[IMU_AXES] = {
        data.accel_x, data.accel_y, data.accel_z,
        data.gyro_x,  data.gyro_y,  data.gyro_z,
    };
    char buf[48];

    for (int i = 0; i < IMU_AXES; i++) {
        if (ok) {
            snprintf(buf, sizeof(buf), "%s: %.2f %s", IMU_AXIS_NAMES[i],
                     (double)values[i], i < 3 ? "m/s2" : "deg/s");
        } else {
            snprintf(buf, sizeof(buf), "%s: Error", IMU_AXIS_NAMES[i]);
        }
        lv_label_set_text(s_imu_lbl[i], buf);
    }
}

static void open_detail_imu(lv_obj_t *content)
{
    const hal_registry_t *reg = hal_get_registry();
    if (!reg || !reg->imu) {
        create_info_row(content, "No IMU registered");
        return;
    }
    const hal_imu_driver_t *imu = reg->imu;
    char buf[64];

    snprintf(buf, sizeof(buf), "Name: %s", imu->name ? imu->name : "Unknown");
    create_info_row(content, buf);
    create_separator(content);

    if (!imu->get_data) {
        create_info_row(content, "(Info only - no settings)");
        return;
    }

    for (int i = 0; i < IMU_AXES; i++) {
        snprintf(buf, sizeof(buf), "%s: --", IMU_AXIS_NAMES[i]);
        s_imu_lbl[i] = create_live_row(content, buf);
        if (i < IMU_AXES - 1) create_separator(content);
    }

    /* The readings move with the board, so keep reading while the page is
     * open, as the GPS page does. Fire immediately. */
    s_imu_timer = lv_timer_create(imu_timer_cb, 2000, NULL);
    lv_timer_ready(s_imu_timer);
}

/* ------------------------------------------------------------------ */
/* Driver detail dispatcher                                             */
/* ------------------------------------------------------------------ */

static void open_driver_detail(driver_type_t type, int index)
{
    static const char *type_names[] = {
        "Display", "Input", "Radio", "GPS", "Audio", "Power", "IMU", "Storage",
    };
    const char *title = ((unsigned)type < (sizeof(type_names)/sizeof(type_names[0])))
                        ? type_names[type] : "Driver";

    /* Hide drivers list; build detail on s_root */
    if (s_sub_screen) {
        lv_obj_add_flag(s_sub_screen, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *outer = NULL;
    lv_obj_t *content = alloc_sub_screen(s_root, title,
                                          back_to_drivers, NULL, &outer);
    s_detail_screen  = outer;
    s_current_screen = SETTINGS_DRIVER_DETAIL;

    switch (type) {
    case DRIVER_TYPE_DISPLAY:  open_detail_display(content);        break;
    case DRIVER_TYPE_INPUT:    open_detail_input(content, index);   break;
    case DRIVER_TYPE_RADIO:    open_detail_radio(content);          break;
    case DRIVER_TYPE_GPS:      open_detail_gps(content);            break;
    case DRIVER_TYPE_AUDIO:    open_detail_audio(content);          break;
    case DRIVER_TYPE_POWER:    open_detail_power(content);          break;
    case DRIVER_TYPE_IMU:      open_detail_imu(content);            break;
    case DRIVER_TYPE_STORAGE:  open_detail_storage(content, index); break;
    }
}

/* ------------------------------------------------------------------ */
/* Drivers list screen                                                  */
/* ------------------------------------------------------------------ */

static void driver_row_clicked_cb(lv_event_t *e)
{
    driver_row_data_t *d = (driver_row_data_t *)lv_event_get_user_data(e);
    if (!d) return;
    open_driver_detail(d->type, d->index);
}

static void add_driver_row(lv_obj_t *list,
                            const char *name,
                            const char *type_label,
                            driver_type_t dtype,
                            int index)
{
    if (s_driver_row_pool_used >= MAX_DRIVER_ROWS) {
        ESP_LOGW(TAG, "driver row pool exhausted");
        return;
    }

    driver_row_data_t *payload = &s_driver_row_pool[s_driver_row_pool_used++];
    payload->type  = dtype;
    payload->index = index;

    const theme_colors_t *tc_drv = theme_get_colors();
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_size(row, LV_PCT(100), ITEM_H);
    lv_obj_set_style_bg_color(row, tc_drv->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, tc_drv->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, tc_drv->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, driver_row_clicked_cb, LV_EVENT_CLICKED, payload);

    /* Driver name (grows) */
    lv_obj_t *lbl_name = lv_label_create(row);
    lv_label_set_text(lbl_name, name ? name : "(unknown)");
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_name, tc_drv->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_name, tc_drv->bg, LV_STATE_PRESSED);
    lv_obj_set_flex_grow(lbl_name, 1);

    /* Type label */
    lv_obj_t *lbl_type = lv_label_create(row);
    lv_label_set_text(lbl_type, type_label ? type_label : "");
    lv_obj_set_style_text_font(lbl_type, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_type, tc_drv->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_type, tc_drv->bg, LV_STATE_PRESSED);

    /* Chevron */
    lv_obj_t *lbl_chev = lv_label_create(row);
    lv_label_set_text(lbl_chev, ">");
    lv_obj_set_style_text_font(lbl_chev, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_chev, tc_drv->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_chev, tc_drv->bg, LV_STATE_PRESSED);
}

static void open_drivers_screen(void)
{
    s_driver_row_pool_used = 0;

    lv_obj_t *content = create_sub_screen("Drivers");
    s_current_screen = SETTINGS_DRIVERS;

    const hal_registry_t *reg = hal_get_registry();
    if (!reg) {
        create_info_row(content, "HAL registry not available");
        return;
    }

    bool any = false;

    if (reg->display) {
        add_driver_row(content, reg->display->name, "Display", DRIVER_TYPE_DISPLAY, 0);
        any = true;
    }

    for (int i = 0; i < (int)reg->input_count; i++) {
        if (!reg->inputs[i]) continue;
        const char *tlbl = reg->inputs[i]->is_touch ? "Touch" : "Keyboard";
        add_driver_row(content, reg->inputs[i]->name, tlbl, DRIVER_TYPE_INPUT, i);
        any = true;
    }

    if (reg->radio) {
        add_driver_row(content, reg->radio->name, "Radio", DRIVER_TYPE_RADIO, 0);
        any = true;
    }

    if (reg->gps) {
        add_driver_row(content, reg->gps->name, "GPS", DRIVER_TYPE_GPS, 0);
        any = true;
    }

    if (reg->audio) {
        add_driver_row(content, reg->audio->name, "Audio", DRIVER_TYPE_AUDIO, 0);
        any = true;
    }

    if (reg->power) {
        add_driver_row(content, reg->power->name, "Power", DRIVER_TYPE_POWER, 0);
        any = true;
    }

    if (reg->imu) {
        add_driver_row(content, reg->imu->name, "IMU", DRIVER_TYPE_IMU, 0);
        any = true;
    }

    for (int i = 0; i < (int)reg->storage_count; i++) {
        if (!reg->storage[i]) continue;
        add_driver_row(content, reg->storage[i]->name, "Storage", DRIVER_TYPE_STORAGE, i);
        any = true;
    }

    if (!any) {
        create_info_row(content, "No drivers registered");
    }
}

/* ------------------------------------------------------------------ */
/* Main list — category row creation                                   */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Power sub-screen                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t seconds;  /* 0: never */
    const char *label;
} power_choice_t;

static const power_choice_t SLEEP_AFTER[] = {
    {60, "1 min"}, {120, "2 min"}, {300, "5 min"},
    {600, "10 min"}, {1800, "30 min"}, {0, "Never"},
};

static const power_choice_t DEEP_SLEEP_AFTER[] = {
    {1800, "30 min"}, {3600, "1 h"}, {7200, "2 h"},
    {21600, "6 h"}, {86400, "24 h"}, {0, "Never"},
};

/* A choice row that wraps onto a second line: six choices do not fit across
 * a 240 px panel. Still one row to choice_check_only(), so one is checked. */
static lv_obj_t *create_wrapping_choice_row(lv_obj_t *content)
{
    lv_obj_t *row = create_choice_row(content);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(row, 4, LV_PART_MAIN);
    return row;
}

/* A line of explanation, wrapped to the width of the screen. */
static void create_note(lv_obj_t *content, const char *text)
{
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_t *lbl = lv_label_create(content);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_pad_left(lbl, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(lbl, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, tc->text, LV_PART_MAIN);
}

static void power_saved_or_warn(esp_err_t err)
{
    if (err != ESP_OK) {
        toast_warn("Not saved for next boot");
    }
}

static void power_sleep_after_cb(lv_event_t *e)
{
    choice_check_only(lv_event_get_current_target(e));
    power_saved_or_warn(power_manager_set_sleep_after(
        (uint32_t)(uintptr_t)lv_event_get_user_data(e)));
}

static void power_deep_sleep_after_cb(lv_event_t *e)
{
    choice_check_only(lv_event_get_current_target(e));
    power_saved_or_warn(power_manager_set_deep_sleep_after(
        (uint32_t)(uintptr_t)lv_event_get_user_data(e)));
}

static void power_wake_on_modem_cb(lv_event_t *e)
{
    choice_check_only(lv_event_get_current_target(e));
    power_saved_or_warn(power_manager_set_wake_on_modem(
        (bool)(uintptr_t)lv_event_get_user_data(e)));
}

static void power_stay_awake_cb(lv_event_t *e)
{
    choice_check_only(lv_event_get_current_target(e));
    power_saved_or_warn(power_manager_set_stay_awake_on_usb(
        (bool)(uintptr_t)lv_event_get_user_data(e)));
}

static void power_request_or_warn(power_request_t request)
{
    if (power_manager_request(request) != ESP_OK) {
        toast_warn("Not available on this device");
    }
}

static void power_sleep_now_cb(lv_event_t *e)
{
    (void)e;
    power_request_or_warn(POWER_REQUEST_SLEEP);
}

static void power_deep_sleep_now_cb(lv_event_t *e)
{
    (void)e;
    power_request_or_warn(POWER_REQUEST_DEEP_SLEEP);
}

static void power_restart_cb(lv_event_t *e)
{
    (void)e;
    power_request_or_warn(POWER_REQUEST_RESTART);
}

static void create_power_choices(lv_obj_t *content, const power_choice_t *choices,
                                 size_t count, uint32_t current, lv_event_cb_t cb)
{
    lv_obj_t *row = create_wrapping_choice_row(content);
    for (size_t i = 0; i < count; i++) {
        create_choice_button(row, choices[i].label, cb, choices[i].seconds,
                             choices[i].seconds == current);
    }
}

static void create_on_off(lv_obj_t *content, bool on, lv_event_cb_t cb)
{
    lv_obj_t *row = create_choice_row(content);
    create_choice_button(row, "On", cb, 1, on);
    create_choice_button(row, "Off", cb, 0, !on);
}

static void open_power_screen(void)
{
    lv_obj_t *content = create_sub_screen("Power");
    s_current_screen = SETTINGS_POWER;

    power_caps_t caps;
    power_manager_get_caps(&caps);
    power_settings_t ps;
    power_manager_get_settings(&ps);

    if (!caps.can_sleep) {
        create_note(content, "Nothing on this board can wake it, so it does not sleep.");
    } else {
        create_info_row(content, "Sleep after");
        create_power_choices(content, SLEEP_AFTER, sizeof(SLEEP_AFTER) / sizeof(SLEEP_AFTER[0]),
                             ps.sleep_after_s, power_sleep_after_cb);
        create_note(content, "Asleep, the screen stays as it is and LoRa keeps listening.");
        create_separator(content);

        if (caps.can_deep_sleep) {
            create_info_row(content, "Deep sleep after");
            create_power_choices(content, DEEP_SLEEP_AFTER,
                                 sizeof(DEEP_SLEEP_AFTER) / sizeof(DEEP_SLEEP_AFTER[0]),
                                 ps.deep_sleep_after_s, power_deep_sleep_after_cb);
            create_note(content, "Deep sleep turns nearly everything off. Waking from it restarts the device.");
            if (caps.can_wake_on_modem) {
                create_info_row(content, "Calls and SMS wake it");
                create_on_off(content, ps.wake_on_modem, power_wake_on_modem_cb);
                create_note(content, "Keeps the modem on in deep sleep, about 2 mA.");
            }
            create_separator(content);
        }

        create_info_row(content, "Stay awake when plugged in");
        create_on_off(content, ps.stay_awake_on_usb, power_stay_awake_cb);
        create_separator(content);

        create_action_row(content, "Sleep now", power_sleep_now_cb);
        if (caps.can_deep_sleep) {
            create_action_row(content, "Deep sleep now", power_deep_sleep_now_cb);
        }
    }
    create_action_row(content, "Restart", power_restart_cb);
    if (caps.has_key) {
        create_note(content, "Power key: press to sleep, hold for the power menu.");
    }
}

/* ------------------------------------------------------------------ */
/* Date & time sub-screen                                               */
/* ------------------------------------------------------------------ */

static lv_obj_t   *s_time_now_lbl    = NULL;  /* "2026-10-13  14:05" */
static lv_obj_t   *s_time_from_lbl   = NULL;  /* "Set by NTP, 5 min ago" */
static lv_obj_t   *s_time_zone_lbl   = NULL;
static lv_obj_t   *s_time_manual     = NULL;  /* shown with automatic time off */
static lv_obj_t   *s_time_ta         = NULL;
static lv_timer_t *s_time_timer      = NULL;

/* The regions the zone picker offers, from the zone names, "Europe" for
 * "Europe/Oslo"; a zone with no region, "UTC", is offered on its own. */
#define TIME_REGIONS_MAX 12
static char s_time_regions[TIME_REGIONS_MAX][16];
static int  s_time_region_count;

static void time_forget_screen(void)
{
    if (s_time_timer) {
        lv_timer_delete(s_time_timer);
        s_time_timer = NULL;
    }
    s_time_now_lbl  = NULL;
    s_time_from_lbl = NULL;
    s_time_zone_lbl = NULL;
    s_time_manual   = NULL;
    s_time_ta       = NULL;
}

static void time_update_labels(void)
{
    if (!s_time_now_lbl) return;
    char date[16], clock[12], line[48];
    time_manager_format_date(date, sizeof(date));
    time_manager_format_time(clock, sizeof(clock));
    snprintf(line, sizeof(line), "%s  %s", date, clock);
    set_label_if_changed(s_time_now_lbl, line);
    time_manager_describe(line, sizeof(line));
    set_label_if_changed(s_time_from_lbl, line);
    time_manager_zone_label(line, sizeof(line));
    set_label_if_changed(s_time_zone_lbl, line);
    /* The status bar reads the clock every 10 s; a new zone or format
     * shows there at once. */
    statusbar_set_time_str(clock);
}

static void time_timer_cb(lv_timer_t *t)
{
    (void)t;
    time_update_labels();
}

static void time_saved_or_warn(esp_err_t err)
{
    if (err != ESP_OK) {
        toast_warn("Not saved for next boot");
    }
}

/* The field the time is typed into: what the clock says now, as typed. */
static void time_fill_field(void)
{
    time_status_t st;
    if (!s_time_ta || time_manager_get_status(&st) != ESP_OK || !st.known) {
        return;
    }
    time_t now = time(NULL);
    struct tm local;
    char text[24];
    localtime_r(&now, &local);
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    lv_textarea_set_text(s_time_ta, text);
}

static void time_auto_cb(lv_event_t *e)
{
    bool on = (bool)(uintptr_t)lv_event_get_user_data(e);
    choice_check_only(lv_event_get_current_target(e));
    time_saved_or_warn(time_manager_set_automatic(on));
    if (s_time_manual) {
        if (on) {
            lv_obj_add_flag(s_time_manual, LV_OBJ_FLAG_HIDDEN);
        } else {
            time_fill_field();
            lv_obj_remove_flag(s_time_manual, LV_OBJ_FLAG_HIDDEN);
        }
    }
    time_update_labels();
}

static void time_24h_cb(lv_event_t *e)
{
    choice_check_only(lv_event_get_current_target(e));
    time_saved_or_warn(time_manager_set_24h((bool)(uintptr_t)lv_event_get_user_data(e)));
    time_update_labels();
}

static void time_set_cb(lv_event_t *e)
{
    (void)e;
    if (!s_time_ta) return;
    esp_err_t err = time_manager_set_local(lv_textarea_get_text(s_time_ta));
    if (err == ESP_ERR_INVALID_ARG) {
        toast_warn("Type it as 2026-10-13 14:05");
    } else if (err != ESP_OK) {
        toast_warn("Time not set");
    } else {
        toast_info("Time set");
    }
    time_update_labels();
}

/* The rows of the zone picker share two styles. Styled one property at a
 * time, as the other pages are, each row cost over a kilobyte of LVGL's
 * 64 KB pool (CONFIG_LV_MEM_SIZE_KILOBYTES), and a region's thirty-odd
 * cities did not fit. Set again for each page, so they follow the theme;
 * nothing uses them by then, as each page is built after the last is gone. */
static lv_style_t s_pick_style;
static lv_style_t s_pick_pressed_style;
static bool       s_pick_styles_ready;

static void pick_styles_init(void)
{
    if (s_pick_styles_ready) {
        lv_style_reset(&s_pick_style);
        lv_style_reset(&s_pick_pressed_style);
    }
    lv_style_init(&s_pick_style);
    lv_style_init(&s_pick_pressed_style);
    s_pick_styles_ready = true;

    const theme_colors_t *tc = theme_get_colors();
    int32_t pad = (ITEM_H - lv_font_get_line_height(&lv_font_montserrat_14)) / 2;
    lv_style_set_width(&s_pick_style, LV_PCT(100));
    lv_style_set_pad_left(&s_pick_style, ITEM_PAD_LEFT);
    lv_style_set_pad_right(&s_pick_style, ITEM_PAD_RIGHT);
    lv_style_set_pad_top(&s_pick_style, pad);
    lv_style_set_pad_bottom(&s_pick_style, pad);
    lv_style_set_text_font(&s_pick_style, &lv_font_montserrat_14);
    lv_style_set_text_color(&s_pick_style, tc->text);
    lv_style_set_bg_color(&s_pick_style, tc->bg);
    lv_style_set_bg_opa(&s_pick_style, LV_OPA_COVER);
    lv_style_set_border_side(&s_pick_style, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&s_pick_style, tc->text);
    lv_style_set_border_width(&s_pick_style, 1);
    lv_style_set_bg_color(&s_pick_pressed_style, tc->primary);
    lv_style_set_text_color(&s_pick_pressed_style, tc->bg);
}

/* A row that opens or picks something, one label, with ">" when it opens.
 * Returns the label. */
static lv_obj_t *create_pick_row(lv_obj_t *content, const char *text, bool opens,
                                 lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = lv_label_create(content);
    lv_label_set_text(row, text);
    lv_obj_add_style(row, &s_pick_style, LV_PART_MAIN);
    lv_obj_add_style(row, &s_pick_pressed_style, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    if (opens) {
        /* Takes the row's font and colour, pressed or not. */
        lv_obj_t *chev = lv_label_create(row);
        lv_label_set_text(chev, ">");
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return row;
}

static void open_zone_screen(const char *region);
static void open_time_screen(void);

/* "Europe/Oslo" in the region "Europe": its part after the region, with
 * spaces for the underscores: "Oslo", "Argentina/Buenos Aires". */
static void zone_city(const char *zone, size_t region_len, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s", zone + region_len + 1);
    for (char *p = out; *p; p++) {
        if (*p == '_') *p = ' ';
    }
}

static bool zone_in_region(const char *zone, const char *region)
{
    size_t n = strlen(region);
    return strncmp(zone, region, n) == 0 && zone[n] == '/';
}

/* The zone picker is a page of its own, and Date & time is closed while it
 * is open, which leaves the pool room for a region's cities. Leaving it goes
 * from a region's cities to the regions, and from the regions to Date & time. */
static void zone_leave(bool to_regions)
{
    if (s_sub_screen) {
        lv_obj_delete(s_sub_screen);
        s_sub_screen = NULL;
    }
    if (to_regions) {
        open_zone_screen(NULL);
    } else {
        open_time_screen();
    }
}

/* The back bar carries the region whose cities are on show, or NULL. */
static void zone_back_cb(lv_event_t *e)
{
    zone_leave(lv_event_get_user_data(e) != NULL);
}

static void zone_pick_cb(lv_event_t *e)
{
    const char *zone = (const char *)lv_event_get_user_data(e);
    time_saved_or_warn(time_manager_set_zone(zone ? zone : ""));
    zone_leave(false);
}

static void zone_region_cb(lv_event_t *e)
{
    const char *region = (const char *)lv_event_get_user_data(e);
    if (s_sub_screen) {
        lv_obj_delete(s_sub_screen);
        s_sub_screen = NULL;
    }
    open_zone_screen(region);
}

/* The regions, with Automatic and UTC first, or the cities of one region.
 * The zone in use is ticked, and so is its region. */
static void open_zone_screen(const char *region)
{
    time_status_t st;
    if (time_manager_get_status(&st) != ESP_OK) {
        st.zone[0] = '\0';
    }
    pick_styles_init();
    lv_obj_t *content = alloc_sub_screen(s_root, region ? region : "Time zone",
                                         zone_back_cb, (void *)region, &s_sub_screen);
    s_current_screen = SETTINGS_TIME_ZONE;
    char text[64];
    size_t count = time_manager_zone_count();

    if (region) {
        size_t n = strlen(region);
        for (size_t i = 0; i < count; i++) {
            const char *zone = time_manager_zone_name(i);
            if (!zone_in_region(zone, region)) continue;
            char city[48];
            zone_city(zone, n, city, sizeof(city));
            bool current = strcmp(zone, st.zone) == 0;
            snprintf(text, sizeof(text), "%s%s", current ? LV_SYMBOL_OK " " : "", city);
            create_pick_row(content, text, false, zone_pick_cb, (void *)zone);
        }
        return;
    }

    snprintf(text, sizeof(text), "%sAutomatic (mobile network)",
             st.zone[0] == '\0' ? LV_SYMBOL_OK " " : "");
    create_pick_row(content, text, false, zone_pick_cb, (void *)"");
    s_time_region_count = 0;
    for (size_t i = 0; i < count; i++) {
        const char *zone = time_manager_zone_name(i);
        const char *slash = strchr(zone, '/');
        if (!slash) {
            bool current = strcmp(zone, st.zone) == 0;
            snprintf(text, sizeof(text), "%s%s", current ? LV_SYMBOL_OK " " : "", zone);
            create_pick_row(content, text, false, zone_pick_cb, (void *)zone);
            continue;
        }
        size_t n = (size_t)(slash - zone);
        if (s_time_region_count > 0
            && strlen(s_time_regions[s_time_region_count - 1]) == n
            && strncmp(s_time_regions[s_time_region_count - 1], zone, n) == 0) {
            continue;  /* the zones are sorted, so a region's come together */
        }
        if (s_time_region_count == TIME_REGIONS_MAX || n >= sizeof(s_time_regions[0])) continue;
        char *r = s_time_regions[s_time_region_count++];
        memcpy(r, zone, n);
        r[n] = '\0';
        bool current = zone_in_region(st.zone, r);
        snprintf(text, sizeof(text), "%s%s", current ? LV_SYMBOL_OK " " : "", r);
        create_pick_row(content, text, true, zone_region_cb, r);
    }
}

static void time_zone_open_cb(lv_event_t *e)
{
    (void)e;
    time_forget_screen();
    zone_leave(true);
}

static void open_time_screen(void)
{
    lv_obj_t *content = create_sub_screen("Date & time");
    s_current_screen = SETTINGS_TIME;

    time_status_t st;
    if (time_manager_get_status(&st) != ESP_OK) {
        create_note(content, "The clock has not started.");
        return;
    }

    s_time_now_lbl  = create_live_row(content, "");
    s_time_from_lbl = create_live_row(content, "");
    if (!st.has_rtc) {
        create_note(content, "No clock chip: sleep and restarts keep the time, "
                             "losing power does not.");
    }
    create_separator(content);

    create_info_row(content, "Set automatically");
    create_on_off(content, st.automatic, time_auto_cb);
    create_note(content, "From the internet, else GPS or the mobile network.");

    /* Typed in, with automatic time off. */
    s_time_manual = lv_obj_create(content);
    style_panel(s_time_manual);
    lv_obj_set_size(s_time_manual, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_time_manual, LV_FLEX_FLOW_COLUMN);
    create_info_row(s_time_manual, "Local time, as 2026-10-13 14:05");
    lv_obj_t *row = create_choice_row(s_time_manual);
    s_time_ta = lv_textarea_create(row);
    lv_textarea_set_one_line(s_time_ta, true);
    lv_textarea_set_max_length(s_time_ta, 19);
    lv_textarea_set_accepted_chars(s_time_ta, "0123456789-: ");
    lv_textarea_set_placeholder_text(s_time_ta, "2026-10-13 14:05");
    lv_obj_set_flex_grow(s_time_ta, 1);
    lv_obj_set_height(s_time_ta, ITEM_H - 4);
    lv_obj_set_style_text_font(s_time_ta, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_time_ta, 2, LV_PART_MAIN);
    create_choice_button(row, "Set", time_set_cb, 0, false);
    if (st.automatic) {
        lv_obj_add_flag(s_time_manual, LV_OBJ_FLAG_HIDDEN);
    } else {
        time_fill_field();
    }
    create_separator(content);

    create_info_row(content, "Time zone");
    pick_styles_init();
    s_time_zone_lbl = create_pick_row(content, "", true, time_zone_open_cb, NULL);
    create_separator(content);

    create_info_row(content, "24-hour clock");
    create_on_off(content, st.format_24h, time_24h_cb);

    time_update_labels();
    s_time_timer = lv_timer_create(time_timer_cb, 2000, NULL);
}

static void item_clicked_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    if (!name) return;

    if (strcmp(name, "WiFi") == 0) {
        lv_obj_add_flag(s_main_list, LV_OBJ_FLAG_HIDDEN);
        open_wifi_screen();
    } else if (strcmp(name, "Mobile data") == 0) {
        lv_obj_add_flag(s_main_list, LV_OBJ_FLAG_HIDDEN);
        open_mobile_screen();
    } else if (strcmp(name, "Bluetooth") == 0) {
        lv_obj_add_flag(s_main_list, LV_OBJ_FLAG_HIDDEN);
        open_bluetooth_screen();
    } else if (strcmp(name, "Appearance") == 0) {
        lv_obj_add_flag(s_main_list, LV_OBJ_FLAG_HIDDEN);
        open_appearance_screen();
    } else if (strcmp(name, "Power") == 0) {
        lv_obj_add_flag(s_main_list, LV_OBJ_FLAG_HIDDEN);
        open_power_screen();
    } else if (strcmp(name, "Date & time") == 0) {
        lv_obj_add_flag(s_main_list, LV_OBJ_FLAG_HIDDEN);
        open_time_screen();
    } else if (strcmp(name, "Drivers") == 0) {
        lv_obj_add_flag(s_main_list, LV_OBJ_FLAG_HIDDEN);
        open_drivers_screen();
    } else if (strcmp(name, "System update") == 0) {
        lv_obj_add_flag(s_main_list, LV_OBJ_FLAG_HIDDEN);
        open_update_screen();
    } else if (strcmp(name, "About") == 0) {
        lv_obj_add_flag(s_main_list, LV_OBJ_FLAG_HIDDEN);
        open_about_screen();
    } else {
        ESP_LOGI(TAG, "Settings: %s — not implemented yet", name);
    }
}

static void create_list_item(lv_obj_t *list, const settings_item_t *item)
{
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_size(row, LV_PCT(100), ITEM_H);
    lv_obj_set_style_bg_color(row, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, ITEM_PAD_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, ITEM_PAD_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, tc->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, tc->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, item_clicked_cb, LV_EVENT_CLICKED,
                        (void *)item->name);

    /* Name label — flex_grow=1 fills available space */
    lv_obj_t *lbl_name = lv_label_create(row);
    lv_label_set_text(lbl_name, item->name);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_name, tc->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_name, tc->bg, LV_STATE_PRESSED);
    lv_obj_set_flex_grow(lbl_name, 1);

    /* Value label (optional) — capture pointers for WiFi and BT */
    if (item->value != NULL) {
        lv_obj_t *lbl_val = lv_label_create(row);
        lv_label_set_text(lbl_val, item->value);
        lv_obj_set_style_text_font(lbl_val, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl_val, tc->text, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl_val, tc->bg, LV_STATE_PRESSED);

        /* Store references for live-update timer */
        if (strcmp(item->name, "WiFi") == 0) {
            s_wifi_value_label = lbl_val;
            lv_label_set_text(lbl_val, wifi_main_value());
        } else if (strcmp(item->name, "Mobile data") == 0) {
            s_cell_value_label = lbl_val;
            lv_label_set_text(lbl_val, cell_main_value());
        } else if (strcmp(item->name, "Bluetooth") == 0) {
            s_bt_value_label = lbl_val;
        }
    }

    /* Chevron */
    lv_obj_t *lbl_chevron = lv_label_create(row);
    lv_label_set_text(lbl_chevron, ">");
    lv_obj_set_style_text_font(lbl_chevron, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_chevron, tc->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_chevron, tc->bg, LV_STATE_PRESSED);
}

/* The Back key: closes the WiFi password dialog, or does what tapping the
 * title bar of the screen in front does. */
static bool settings_back(void)
{
    if (s_wifi_pwd_ctx.dialog) {
        lv_obj_delete(s_wifi_pwd_ctx.dialog);
        return true;
    }
    lv_obj_t *screen = s_detail_screen ? s_detail_screen : s_sub_screen;
    if (!screen) return false;
    lv_obj_send_event(lv_obj_get_user_data(screen), LV_EVENT_CLICKED, NULL);
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t settings_ui_create(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "Creating settings UI");

    if (parent == NULL) {
        parent = lv_scr_act();
    }

    lv_obj_update_layout(parent);
    s_app_w = lv_obj_get_width(parent);
    s_app_h = lv_obj_get_height(parent);
    if (s_app_w == 0) s_app_w = 240;
    if (s_app_h == 0) s_app_h = 296;

    /* Root container — fills the entire app area, transparent bg */
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    /* ----------------------------------------------------------------
     * Title bar (30px)
     * ---------------------------------------------------------------- */
    lv_obj_t *title_bar = lv_obj_create(s_root);
    lv_obj_set_pos(title_bar, 0, 0);
    lv_obj_set_size(title_bar, s_app_w, TITLE_BAR_H);
    style_title_bar(title_bar);

    const theme_colors_t *tc_main = theme_get_colors();

    lv_obj_t *lbl_title = lv_label_create(title_bar);
    lv_label_set_text(lbl_title, "Settings");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_title, tc_main->text, LV_PART_MAIN);
    lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, 0, 0);

    /* ----------------------------------------------------------------
     * Scrollable list container below title bar
     * ---------------------------------------------------------------- */
    s_main_list = lv_obj_create(s_root);
    lv_obj_set_pos(s_main_list, 0, TITLE_BAR_H);
    lv_obj_set_size(s_main_list, s_app_w, s_app_h - TITLE_BAR_H);
    lv_obj_set_style_bg_color(s_main_list, tc_main->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_main_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_main_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_main_list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_main_list, 0, LV_PART_MAIN);

    lv_obj_set_flex_flow(s_main_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_main_list,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_set_scrollbar_mode(s_main_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_main_list, tc_main->text, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_main_list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_main_list, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_main_list, 0, LV_PART_SCROLLBAR);

    /* Reset value label pointers before rebuilding */
    s_wifi_value_label = NULL;
    s_cell_value_label = NULL;
    s_bt_value_label   = NULL;

    bool has_modem = net_cellular_present();
    for (size_t i = 0; i < ITEMS_COUNT; i++) {
        if (!has_modem && strcmp(s_items[i].name, "Mobile data") == 0) {
            continue;
        }
        create_list_item(s_main_list, &s_items[i]);
    }

    s_current_screen = SETTINGS_MAIN;
    s_sub_screen     = NULL;
    s_detail_screen  = NULL;
    ui_manager_set_back_handler(s_root, settings_back);

    /* Start the main list live-update timer (2 s interval) */
    if (s_main_timer) {
        lv_timer_delete(s_main_timer);
    }
    s_main_timer = lv_timer_create(settings_main_update_timer, 2000, NULL);

    return ESP_OK;
}

void settings_ui_show(void)
{
    if (s_root) {
        lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
}

void settings_ui_hide(void)
{
    if (s_root) {
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
}

void settings_ui_destroy(void)
{
    /* Stop all live-update timers */
    if (s_main_timer) {
        lv_timer_delete(s_main_timer);
        s_main_timer = NULL;
    }
    if (s_power_timer) {
        lv_timer_delete(s_power_timer);
        s_power_timer     = NULL;
        s_power_batt_lbl  = NULL;
        s_power_pct_lbl   = NULL;
        s_power_state_lbl = NULL;
    }
    if (s_gps_timer) {
        lv_timer_delete(s_gps_timer);
        s_gps_timer      = NULL;
        s_gps_status_lbl = NULL;
        s_gps_lat_lbl    = NULL;
        s_gps_lon_lbl    = NULL;
        s_gps_sat_lbl    = NULL;
        s_gps_sig_lbl    = NULL;
        s_gps_orbit_lbl  = NULL;
    }
    if (s_imu_timer) {
        lv_timer_delete(s_imu_timer);
        s_imu_timer = NULL;
        memset(s_imu_lbl, 0, sizeof(s_imu_lbl));
    }

    cell_forget_screen();
    time_forget_screen();
    update_forget_screen();

    /* Nullify label pointers — LVGL will clean up widgets when s_root is deleted */
    s_wifi_value_label   = NULL;
    s_cell_value_label   = NULL;
    s_bt_value_label     = NULL;
    s_wifi_status_label  = NULL;
    s_wifi_scan_list     = NULL;
    s_ble_status_label   = NULL;
    s_ble_name_label     = NULL;
    s_ble_peer_label     = NULL;
    s_ble_pairing_label  = NULL;
    s_ble_toggle_btn_lbl = NULL;

    if (s_root) {
        lv_obj_delete(s_root);
        s_root          = NULL;
        s_main_list     = NULL;
        s_sub_screen    = NULL;
        s_detail_screen = NULL;
    }

    s_current_screen       = SETTINGS_MAIN;
    s_driver_row_pool_used = 0;
}
