#include "ui/statusbar.h"
#include "ui/call_ui.h"
#include "ui/theme.h"
#include "thistle/wifi_manager.h"
#include "thistle/time_manager.h"
#include "thistle/net_manager.h"
#include "thistle/mesh.h"
#include "thistle/sms.h"
#include "thistle/msg_alert.h"
#include "thistle/tailnet.h"
#include "ui/toast.h"
#include "thistle/app_manager.h"
#include "hal/board.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "statusbar";

#define STATUSBAR_HEIGHT 24

/* How often the clock, battery and network readings are refreshed. A label
 * is only rewritten when its text changes, so on e-paper a tick that finds
 * nothing new costs no refresh. */
#define STATUSBAR_UPDATE_MS 10000

/* How often the unread-message count, the call chip and the tailnet are
 * checked. Each is a number or two read from the kernel, and a label is
 * only touched when it changes, so a message shows within a second at no
 * cost while none arrive, and a call's time on e-paper, in whole minutes,
 * costs a refresh a minute. */
#define STATUSBAR_MESSAGES_MS 1000

/* Narrower than this, the bar has no room for the call chip beside the
 * envelope, the network, the battery and the clock: a 240 px T-Deck Max
 * overflowed. The network reading gives way while the chip shows. */
#define STATUSBAR_ROOMY_W 300

/* Where a tap on the unread-message count goes. */
#define MESSENGER_APP_ID "com.thistle.messenger"

/* The battery when the power driver cannot say. */
#define BATTERY_UNKNOWN "--% " LV_SYMBOL_BATTERY_EMPTY

static void statusbar_update_cb(lv_timer_t *timer);
static void statusbar_messages_cb(lv_timer_t *timer);
static lv_timer_t *s_update_timer   = NULL;
static lv_timer_t *s_messages_timer = NULL;

/* LVGL object handles */
static lv_obj_t *s_container     = NULL;
static lv_obj_t *s_title_label   = NULL;
static lv_obj_t *s_time_label    = NULL;
static lv_obj_t *s_messages_label = NULL;
static lv_obj_t *s_call_label    = NULL;
static lv_obj_t *s_battery_label = NULL;
static lv_obj_t *s_wifi_label    = NULL;
static lv_obj_t *s_tailnet_label = NULL;
/* The widest time in the clock's format, which the time label is sized to. */
static const char *s_time_widest = NULL;

/* Never narrower than a time in the clock's format, so when the bar is
 * crowded the title gives way rather than the clock wrapping onto a second
 * line. Sized to the format rather than the text, so the bar does not shift
 * each minute as the digits change. */
static void fit_time_label(const char *time_str)
{
    const char *widest = strlen(time_str) > 5 ? "00:00 PM" : "00:00";
    if (widest == s_time_widest) {
        return;
    }
    s_time_widest = widest;
    const lv_font_t *font = lv_obj_get_style_text_font(s_time_label, LV_PART_MAIN);
    lv_obj_set_style_min_width(s_time_label,
                               lv_text_get_width(widest, (uint32_t)strlen(widest), font, 0),
                               LV_PART_MAIN);
}

static void messages_tap_cb(lv_event_t *e)
{
    (void)e;
    app_manager_launch(MESSENGER_APP_ID);
}

static void call_tap_cb(lv_event_t *e)
{
    (void)e;
    call_ui_chip_tapped();
}

esp_err_t statusbar_create(lv_obj_t *parent)
{
    if (parent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const theme_colors_t *colors = theme_get_colors();

    /* Main container — full width, fixed height, flex-row layout */
    s_container = lv_obj_create(parent);
    lv_obj_set_size(s_container, LV_PCT(100), STATUSBAR_HEIGHT);
    lv_obj_set_pos(s_container, 0, 0);
    lv_obj_set_style_bg_color(s_container, colors->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_container, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_container, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(s_container, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_container, colors->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_container, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_container, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_container, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_container, 0, LV_PART_MAIN);

    /* Flex row layout */
    lv_obj_set_layout(s_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_container,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Title label — left */
    s_title_label = lv_label_create(s_container);
    lv_label_set_text(s_title_label, "ThistleOS");
    lv_obj_set_style_text_color(s_title_label, colors->text, LV_PART_MAIN);
    lv_obj_set_flex_grow(s_title_label, 1);
    /* One line, shortened if it must be: on a 240 px screen a wrapped title
     * spills out of the bar. */
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_title_label,
                      lv_font_get_line_height(lv_obj_get_style_text_font(s_title_label,
                                                                         LV_PART_MAIN)));

    /* Time label — center */
    s_time_label = lv_label_create(s_container);
    s_time_widest = NULL;
    lv_obj_set_style_text_color(s_time_label, colors->text, LV_PART_MAIN);
    lv_obj_set_flex_grow(s_time_label, 1);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    {
        char init_time[12];
        time_manager_format_time(init_time, sizeof(init_time));
        lv_label_set_text(s_time_label, init_time);
        fit_time_label(init_time);
    }

    /* Right-side cluster: wifi + battery */
    lv_obj_t *right = lv_obj_create(s_container);
    /* As wide as its labels: at the default width a transport name such as
     * "Host Network" is cut off at the left. Packed from the start, because
     * LVGL sizes a container to its content from the right-most child edge:
     * packed from the end, labels that outgrow it spill off its left side,
     * which that measure never sees, and it never widens. */
    lv_obj_set_size(right, LV_SIZE_CONTENT, STATUSBAR_HEIGHT - 2);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(right, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(right, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(right, 0, LV_PART_MAIN);
    lv_obj_set_layout(right, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 4, LV_PART_MAIN);

    /* A call tucked away, with its time, or the calls missed: a phone and
     * a number, hidden when there is neither. A tap brings the call back, or
     * opens the Phone app. */
    s_call_label = lv_label_create(right);
    lv_label_set_text(s_call_label, "");
    lv_obj_set_style_text_color(s_call_label, colors->text, LV_PART_MAIN);
    lv_obj_add_flag(s_call_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_call_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_call_label, 6);
    lv_obj_add_event_cb(s_call_label, call_tap_cb, LV_EVENT_CLICKED, NULL);

    /* Unread messages: an envelope and how many, hidden while there are
     * none. A tap opens the Messenger; the label is small, so the area that
     * takes the tap reaches past it. */
    s_messages_label = lv_label_create(right);
    lv_label_set_text(s_messages_label, "");
    lv_obj_set_style_text_color(s_messages_label, colors->text, LV_PART_MAIN);
    lv_obj_add_flag(s_messages_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_messages_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_messages_label, 6);
    lv_obj_add_event_cb(s_messages_label, messages_tap_cb, LV_EVENT_CLICKED, NULL);

    s_wifi_label = lv_label_create(right);
    lv_label_set_text(s_wifi_label, "");
    lv_obj_set_style_text_color(s_wifi_label, colors->text, LV_PART_MAIN);

    /* On the tailnet: Tailscale's symbol, as in the drawer and the quick
     * panel, beside the network it runs over; hidden otherwise. */
    s_tailnet_label = lv_label_create(right);
    lv_label_set_text(s_tailnet_label, LV_SYMBOL_SHUFFLE);
    lv_obj_set_style_text_color(s_tailnet_label, colors->text, LV_PART_MAIN);
    lv_obj_add_flag(s_tailnet_label, LV_OBJ_FLAG_HIDDEN);

    s_battery_label = lv_label_create(right);
    lv_label_set_text(s_battery_label, BATTERY_UNKNOWN);
    lv_obj_set_style_text_color(s_battery_label, colors->text, LV_PART_MAIN);

    /* An LVGL timer rather than an esp_timer, so the readings are written
     * from inside lv_timer_handler() with the LVGL lock held. Ready at once,
     * so the first readings replace the placeholders on the first pass. */
    if (s_update_timer == NULL) {
        s_update_timer = lv_timer_create(statusbar_update_cb, STATUSBAR_UPDATE_MS, NULL);
        lv_timer_ready(s_update_timer);
    }
    if (s_messages_timer == NULL) {
        s_messages_timer = lv_timer_create(statusbar_messages_cb, STATUSBAR_MESSAGES_MS, NULL);
        lv_timer_ready(s_messages_timer);
    }

    ESP_LOGI(TAG, "status bar created (%d px)", STATUSBAR_HEIGHT);
    return ESP_OK;
}

/* Rewrite a label only if its text changed: on e-paper every rewrite is a
 * panel refresh. */
static void set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

/* A battery with as many bars as it has charge. */
static const char *battery_symbol(uint8_t percent)
{
    if (percent >= 90) return LV_SYMBOL_BATTERY_FULL;
    if (percent >= 65) return LV_SYMBOL_BATTERY_3;
    if (percent >= 40) return LV_SYMBOL_BATTERY_2;
    if (percent >= 15) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

/* "72%" and a battery, or a lightning bolt while it charges. */
void statusbar_set_battery(uint8_t percent, bool charging)
{
    if (s_battery_label == NULL) {
        return;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%% %s", percent,
             charging ? LV_SYMBOL_CHARGE : battery_symbol(percent));
    set_text_if_changed(s_battery_label, buf);
}

void statusbar_set_wifi(bool connected, int rssi)
{
    (void)rssi;
    if (s_wifi_label == NULL) {
        return;
    }
    set_text_if_changed(s_wifi_label, connected ? LV_SYMBOL_WIFI : "");
}

void statusbar_set_title(const char *title)
{
    if (s_title_label == NULL || title == NULL) {
        return;
    }
    lv_label_set_text(s_title_label, title);
}

void statusbar_set_time(uint8_t hour, uint8_t minute)
{
    if (s_time_label == NULL) {
        return;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    lv_label_set_text(s_time_label, buf);
}

uint16_t statusbar_get_height(void)
{
    return STATUSBAR_HEIGHT;
}

void statusbar_set_time_str(const char *time_str)
{
    if (s_time_label == NULL || time_str == NULL) {
        return;
    }
    set_text_if_changed(s_time_label, time_str);
    fit_time_label(time_str);
}

void statusbar_refresh_theme(void)
{
    const theme_colors_t *colors = theme_get_colors();
    if (s_container) {
        lv_obj_set_style_bg_color(s_container, colors->bg, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_container, colors->text, LV_PART_MAIN);
    }
    if (s_title_label)   lv_obj_set_style_text_color(s_title_label,   colors->text, LV_PART_MAIN);
    if (s_time_label)    lv_obj_set_style_text_color(s_time_label,    colors->text, LV_PART_MAIN);
    if (s_messages_label) lv_obj_set_style_text_color(s_messages_label, colors->text, LV_PART_MAIN);
    if (s_call_label)    lv_obj_set_style_text_color(s_call_label,    colors->text, LV_PART_MAIN);
    if (s_battery_label) lv_obj_set_style_text_color(s_battery_label, colors->text, LV_PART_MAIN);
    if (s_wifi_label)    lv_obj_set_style_text_color(s_wifi_label,    colors->text, LV_PART_MAIN);
    if (s_tailnet_label) lv_obj_set_style_text_color(s_tailnet_label, colors->text, LV_PART_MAIN);
}

/* ------------------------------------------------------------------ */
/* Periodic update (LVGL timer, created by statusbar_create)           */
/* ------------------------------------------------------------------ */

static void update_battery(void)
{
    const hal_registry_t *reg = hal_get_registry();
    if (!reg || !reg->power || !s_battery_label) {
        return;
    }

    /* get_info is the one call that says whether the driver is up at all.
     * The single-value calls answer 0 from a driver that never found its
     * chip, which would read as an empty battery rather than an unknown one. */
    if (reg->power->get_info) {
        hal_power_info_t info = {0};
        if (reg->power->get_info(&info) != ESP_OK ||
            info.state == HAL_POWER_STATE_NO_BATTERY) {
            set_text_if_changed(s_battery_label, BATTERY_UNKNOWN);
            return;
        }
        statusbar_set_battery(info.percent, info.state == HAL_POWER_STATE_CHARGING);
        return;
    }

    uint8_t pct = reg->power->get_battery_percent ? reg->power->get_battery_percent() : 0;
    bool charging = reg->power->is_charging ? reg->power->is_charging() : false;
    statusbar_set_battery(pct, charging);
}

/* How long a new message's pop-up stays up */
#define MESSAGE_POPUP_MS 5000

/* Pop up the latest message the kernel alerted for (thistle/msg_alert.h),
 * which has buzzed for it already. Any that came before it are counted. */
static void show_message_alert(void)
{
    msg_alert_t alert;
    if (!rs_msg_alert_take(&alert)) {
        return;
    }
    /* Room for the longest name, preview and count; in practice it is well
     * under the 127 bytes a toast shows. */
    char text[160];
    if (alert.count > 1) {
        snprintf(text, sizeof(text), LV_SYMBOL_ENVELOPE " %s: %s (+%u more)",
                 alert.from, alert.text, (unsigned)(alert.count - 1));
    } else {
        snprintf(text, sizeof(text), LV_SYMBOL_ENVELOPE " %s: %s", alert.from, alert.text);
    }
    toast_show(text, TOAST_INFO, MESSAGE_POPUP_MS);
}

static void set_hidden(lv_obj_t *obj, bool hide)
{
    if (obj && hide != lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        if (hide) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void statusbar_set_clock_visible(bool visible)
{
    set_hidden(s_time_label, !visible);
}

/* The call chip (call_ui.h), shown, rewritten and hidden only on change. On
 * a narrow screen the network reading makes room for it: the clock and the
 * battery stay. A call has the modem to itself anyway. */
static void update_call_chip(void)
{
    if (s_call_label == NULL) {
        return;
    }
    char text[24];
    bool shown = call_ui_chip_text(text, sizeof(text));
    if (shown) {
        set_text_if_changed(s_call_label, text);
    }
    set_hidden(s_call_label, !shown);
    set_hidden(s_wifi_label,
               shown && lv_obj_get_width(s_container) < STATUSBAR_ROOMY_W);
}

/* Tailscale's symbol while the device is on the tailnet. On a narrow screen
 * it gives way to the call chip, as the network does: a call suspends the
 * data session the tailnet runs over. */
static void update_tailnet(void)
{
    if (s_tailnet_label == NULL) {
        return;
    }
    bool connected = tailnet_is_connected();
    bool call_shown = !lv_obj_has_flag(s_call_label, LV_OBJ_FLAG_HIDDEN);
    bool hide = !connected || (call_shown && lv_obj_get_width(s_container) < STATUSBAR_ROOMY_W);
    if (hide != lv_obj_has_flag(s_tailnet_label, LV_OBJ_FLAG_HIDDEN)) {
        ESP_LOGI(TAG, "tailnet %s", hide ? "hidden" : "shown");
        set_hidden(s_tailnet_label, hide);
    }
}

/* Show how many mesh messages and SMS are unread, or nothing when none are,
 * and pop up any message that has just arrived; and the call chip and the
 * tailnet, which each read a value or two from the kernel. */
static void statusbar_messages_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_messages_label == NULL) {
        return;
    }

    show_message_alert();
    update_call_chip();
    update_tailnet();

    uint32_t unread = rs_mesh_unread_count() + rs_sms_unread_count();
    bool hidden = lv_obj_has_flag(s_messages_label, LV_OBJ_FLAG_HIDDEN);
    if (unread == 0) {
        if (!hidden) {
            lv_obj_add_flag(s_messages_label, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    char buf[16];
    if (unread > 99) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_ENVELOPE " 99+");
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_ENVELOPE " %u", (unsigned)unread);
    }
    set_text_if_changed(s_messages_label, buf);
    if (hidden) {
        lv_obj_clear_flag(s_messages_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void statusbar_update_cb(lv_timer_t *timer)
{
    (void)timer;

    char time_buf[12];
    time_manager_format_time(time_buf, sizeof(time_buf));
    statusbar_set_time_str(time_buf);

    update_battery();

    /* The network traffic takes, not the tailnet over it: WiFi's symbol, or
     * the mobile network's name ("LTE"), or nothing while there is none. The
     * simulator's host network shows as WiFi. */
    if (s_wifi_label) {
        const hal_net_driver_t *net = net_is_connected() ? net_get_active_underlay() : NULL;
        char buf[24] = "";
        if (net && net->type == HAL_NET_CELLULAR) {
            snprintf(buf, sizeof(buf), "%s", net->name ? net->name : "LTE");
        } else if (net && net->type == HAL_NET_BLE) {
            snprintf(buf, sizeof(buf), "%s", LV_SYMBOL_BLUETOOTH);
        } else if (net) {
            snprintf(buf, sizeof(buf), "%s", LV_SYMBOL_WIFI);
        }
        set_text_if_changed(s_wifi_label, buf);
    }
}
