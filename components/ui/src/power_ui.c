/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the sleep screen and the power menu
 *
 * The power manager (thistle/power_manager.h) asks; this shows. Its requests
 * are taken from an LVGL timer, so everything here runs inside
 * lv_timer_handler() with the LVGL lock held, as the Home key's handling does.
 *
 * The sleep screen stays on the e-paper for as long as the device sleeps,
 * and through deep sleep, since the panel keeps its picture with no power.
 * It is drawn at once rather than at LVGL's next refresh, so that the panel's
 * own refresh, and with it the sleep, is not held up.
 */

#include "ui/power_ui.h"
#include "ui/epaper_refresh.h"
#include "ui/manager.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "hal/board.h"
#include "thistle/power_manager.h"

#include "esp_log.h"
#include "lvgl.h"

#include <stdint.h>
#include <stdio.h>

static const char *TAG = "power_ui";

#define REQUEST_POLL_MS 100

#define MENU_MAX_W     220
#define MENU_MARGIN      8
#define MENU_BORDER      2
#define MENU_PAD         6
#define MENU_BTN_H      30
#define MENU_BTN_GAP     6

/* A menu choice that is not a power request. */
#define CHOICE_CANCEL ((uintptr_t)-1)

static lv_obj_t *s_sleep_screen;
static lv_obj_t *s_menu;  /* the backdrop; the dialog is its child */

static void close_menu(void)
{
    if (s_menu) {
        lv_obj_delete(s_menu);
        s_menu = NULL;
    }
}

bool power_ui_close_menu(void)
{
    bool open = s_menu != NULL;
    close_menu();
    return open;
}

bool power_ui_sleep_screen_shown(void)
{
    return s_sleep_screen != NULL;
}

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    return label;
}

static void show_sleep_screen(void)
{
    close_menu();
    toast_dismiss();
    lv_obj_t *screen = ui_manager_get_screen();
    if (s_sleep_screen || !screen) {
        return;
    }

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(panel, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, 10, LV_PART_MAIN);

    add_label(panel, "Asleep", &lv_font_montserrat_22);

    power_caps_t caps;
    power_manager_get_caps(&caps);
    add_label(panel,
              caps.has_key ? "Press the power key\nor any key to wake"
                           : "Press any key to wake",
              &lv_font_montserrat_14);

    const hal_registry_t *reg = hal_get_registry();
    hal_power_info_t info;
    if (reg && reg->power && reg->power->get_info && reg->power->get_info(&info) == ESP_OK
        && info.state != HAL_POWER_STATE_NO_BATTERY) {
        char battery[24];
        snprintf(battery, sizeof(battery), "Battery %u%%", (unsigned)info.percent);
        add_label(panel, battery, &lv_font_montserrat_14);
    }

    lv_obj_move_foreground(panel);
    s_sleep_screen = panel;

    /* A full refresh, so the panel keeps a clean picture for however long
     * the sleep lasts; drawn now, so that refresh starts at once. */
    epaper_refresh_request_full();
    lv_refr_now(NULL);
}

static void hide_sleep_screen(void)
{
    close_menu();
    if (!s_sleep_screen) {
        return;
    }
    lv_obj_delete(s_sleep_screen);
    s_sleep_screen = NULL;
    ui_manager_force_refresh();
}

static void show_held_off(void)
{
    const char *why = power_manager_held_by();
    char text[64];
    snprintf(text, sizeof(text), "Not sleeping: %s", why ? why : "the device is busy");
    toast_show(text, TOAST_INFO, 3000);
}

static void menu_choice_cb(lv_event_t *e)
{
    uintptr_t choice = (uintptr_t)lv_event_get_user_data(e);
    close_menu();
    if (choice == CHOICE_CANCEL) {
        return;
    }
    if (power_manager_request((power_request_t)choice) != ESP_OK) {
        toast_warn("Not available on this device");
    }
}

static void menu_key_cb(lv_event_t *e)
{
    if (lv_event_get_key(e) == LV_KEY_ESC) {
        close_menu();
    }
}

static void backdrop_clicked_cb(lv_event_t *e)
{
    /* Only a tap outside the dialog, on the backdrop itself. */
    if (lv_event_get_target(e) == s_menu) {
        close_menu();
    }
}

static void add_choice(lv_obj_t *dialog, const char *text, uintptr_t choice, int32_t width)
{
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_t *btn = lv_obj_create(dialog);
    lv_obj_set_size(btn, width, MENU_BTN_H);
    lv_obj_set_style_bg_color(btn, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, tc->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, tc->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    /* The keyboard moves a thick border; with no colour to spare on e-paper,
     * that is what shows which choice Enter takes. */
    lv_obj_set_style_border_width(btn, 3, LV_STATE_FOCUS_KEY);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, menu_choice_cb, LV_EVENT_CLICKED, (void *)choice);
    lv_obj_add_event_cb(btn, menu_key_cb, LV_EVENT_KEY, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, tc->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, tc->bg, LV_STATE_PRESSED);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    lv_group_t *group = lv_group_get_default();
    if (group) {
        lv_group_add_obj(group, btn);
    }
}

static void show_menu(void)
{
    lv_obj_t *screen = ui_manager_get_screen();
    if (s_menu || s_sleep_screen || !screen) {
        return;
    }
    const theme_colors_t *tc = theme_get_colors();
    power_caps_t caps;
    power_manager_get_caps(&caps);

    /* A backdrop that takes taps outside the dialog. Clear, not dimmed: a
     * dimmed screen dithers on e-paper. */
    lv_obj_t *backdrop = lv_obj_create(screen);
    lv_obj_set_pos(backdrop, 0, 0);
    lv_obj_set_size(backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(backdrop, 0, LV_PART_MAIN);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(backdrop, backdrop_clicked_cb, LV_EVENT_CLICKED, NULL);
    s_menu = backdrop;

    int32_t screen_w = lv_obj_get_width(screen);
    int32_t dialog_w = screen_w - 2 * MENU_MARGIN;
    if (dialog_w > MENU_MAX_W) dialog_w = MENU_MAX_W;
    int32_t content_w = dialog_w - 2 * (MENU_BORDER + MENU_PAD);

    lv_obj_t *dialog = lv_obj_create(backdrop);
    lv_obj_set_width(dialog, dialog_w);
    lv_obj_set_height(dialog, LV_SIZE_CONTENT);
    lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dialog, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dialog, MENU_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dialog, tc->text, LV_PART_MAIN);
    lv_obj_set_style_radius(dialog, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dialog, MENU_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(dialog, MENU_BTN_GAP, LV_PART_MAIN);
    lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dialog, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(dialog);
    lv_label_set_text(title, "Power");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, tc->text, LV_PART_MAIN);

    if (caps.can_sleep) {
        add_choice(dialog, "Sleep", POWER_REQUEST_SLEEP, content_w);
    }
    if (caps.can_deep_sleep) {
        add_choice(dialog, "Deep sleep", POWER_REQUEST_DEEP_SLEEP, content_w);
    }
    add_choice(dialog, "Restart", POWER_REQUEST_RESTART, content_w);
    add_choice(dialog, "Cancel", CHOICE_CANCEL, content_w);

    lv_obj_move_foreground(backdrop);
    lv_group_t *group = lv_group_get_default();
    lv_obj_t *first = lv_obj_get_child(dialog, 1);
    if (group && first) {
        lv_group_focus_obj(first);
    }
}

static void request_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    switch (power_ui_take_request()) {
    case POWER_UI_SHOW_SLEEP_SCREEN:
        show_sleep_screen();
        break;
    case POWER_UI_HIDE_SLEEP_SCREEN:
        hide_sleep_screen();
        break;
    case POWER_UI_SHOW_MENU:
        show_menu();
        break;
    case POWER_UI_SHOW_HELD_OFF:
        show_held_off();
        break;
    default:
        break;
    }
}

esp_err_t power_ui_init(void)
{
    if (!lv_timer_create(request_timer_cb, REQUEST_POLL_MS, NULL)) {
        ESP_LOGW(TAG, "no timer; the sleep screen and power menu will not show");
        return ESP_ERR_NO_MEM;
    }
    power_ui_set_menu_available(true);
    return ESP_OK;
}
