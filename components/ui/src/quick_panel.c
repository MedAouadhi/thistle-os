/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the quick panel
 *
 * Built when opened and deleted when closed, so it costs LVGL's 64 KB pool
 * nothing while shut. Everything in it shares a handful of styles, set up
 * afresh at each opening so a theme changed meanwhile shows. "On" is LVGL's
 * checked state, drawn inverted, which reads in black and white.
 *
 * It hangs below the status bar, which stays visible. A clear backdrop over
 * the rest of the screen takes the tap that closes it: clear, not dimmed,
 * because a dimmed screen dithers on e-paper.
 *
 * Everything here runs on the LVGL task with the LVGL lock held: the UI
 * manager opens and closes the panel from its key timer, as it does Home.
 */

#include "ui/quick_panel.h"
#include "ui/manager.h"
#include "ui/power_ui.h"
#include "ui/statusbar.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "thistle/app_manager.h"

#include "esp_log.h"
#include "lvgl.h"

#include <stdint.h>

static const char *TAG = "quick_panel";

#define SETTINGS_APP_ID "com.thistle.settings"

#define PANEL_PAD      6
#define GAP            6
#define TILE_MIN_W    70
#define TILE_H        44
#define FOOTER_H      28
#define SLIDER_H      10
#define SYMBOL_W      24
/* Below the panel, always some backdrop to tap. */
#define CLOSE_STRIP   16
/* How often an open panel asks each switch where it is. A switch whose
 * answer has not changed is not redrawn, so on e-paper a quiet second costs
 * no refresh. */
#define REFRESH_MS  1000

typedef struct {
    const quick_toggle_t *def;
    lv_obj_t *tile;
} toggle_ui_t;

typedef struct {
    const quick_level_t *def;
    lv_obj_t *slider;
} level_ui_t;

static const quick_toggle_t *s_toggles[QUICK_PANEL_MAX_TOGGLES];
static int s_toggle_count;
static const quick_level_t *s_levels[QUICK_PANEL_MAX_LEVELS];
static int s_level_count;

/* The panel while it is open. */
static lv_obj_t *s_backdrop;
static lv_timer_t *s_refresh_timer;
static toggle_ui_t s_toggle_ui[QUICK_PANEL_MAX_TOGGLES];
static int s_toggle_ui_count;
static level_ui_t s_level_ui[QUICK_PANEL_MAX_LEVELS];
static int s_level_ui_count;
static volatile bool s_open;

/* The keys go to the panel alone while it is open, not to the app under it. */
static lv_group_t *s_group;
static lv_indev_t *s_keypad;
static lv_group_t *s_keypad_group_before;

static lv_style_t s_style_box;       /* a plain container: no fill, no border, no padding */
static lv_style_t s_style_panel;
static lv_style_t s_style_tile;
static lv_style_t s_style_tile_on;
static lv_style_t s_style_tile_pressed;
static lv_style_t s_style_focus;
static lv_style_t s_style_symbol;
static lv_style_t s_style_text;
static lv_style_t s_style_tile_label;  /* as wide as a tile allows, then "..." */
static lv_style_t s_style_slider;
static lv_style_t s_style_slider_ind;
static lv_style_t s_style_slider_knob;

/* ------------------------------------------------------------------ */
/* The registry                                                         */
/* ------------------------------------------------------------------ */

esp_err_t quick_panel_add_toggle(const quick_toggle_t *toggle)
{
    if (!toggle || !toggle->label || !toggle->is_on || !toggle->set_on) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_toggle_count == QUICK_PANEL_MAX_TOGGLES) {
        ESP_LOGW(TAG, "no room for \"%s\"", toggle->label);
        return ESP_ERR_NO_MEM;
    }
    s_toggles[s_toggle_count++] = toggle;
    return ESP_OK;
}

esp_err_t quick_panel_add_level(const quick_level_t *level)
{
    if (!level || !level->label || !level->get || !level->set) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_level_count == QUICK_PANEL_MAX_LEVELS) {
        ESP_LOGW(TAG, "no room for \"%s\"", level->label);
        return ESP_ERR_NO_MEM;
    }
    s_levels[s_level_count++] = level;
    return ESP_OK;
}

bool quick_panel_is_open(void)
{
    return s_open;
}

/* ------------------------------------------------------------------ */
/* Showing state                                                        */
/* ------------------------------------------------------------------ */

static void show_toggle(const toggle_ui_t *ui)
{
    bool on = ui->def->is_on();
    if (on != lv_obj_has_state(ui->tile, LV_STATE_CHECKED)) {
        if (on) {
            lv_obj_add_state(ui->tile, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(ui->tile, LV_STATE_CHECKED);
        }
    }
}

static void show_level(const level_ui_t *ui)
{
    if (lv_slider_is_dragged(ui->slider)) {
        return;
    }
    int32_t level = ui->def->get();
    if (level != lv_slider_get_value(ui->slider)) {
        lv_slider_set_value(ui->slider, level, LV_ANIM_OFF);
    }
}

/* One switch can move another (a volume raised unmutes), so after any
 * change everything is asked again. */
static void show_all(void)
{
    for (int i = 0; i < s_toggle_ui_count; i++) {
        show_toggle(&s_toggle_ui[i]);
    }
    for (int i = 0; i < s_level_ui_count; i++) {
        show_level(&s_level_ui[i]);
    }
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    show_all();
}

/* ------------------------------------------------------------------ */
/* Events                                                               */
/* ------------------------------------------------------------------ */

static void toggle_clicked_cb(lv_event_t *e)
{
    toggle_ui_t *ui = lv_event_get_user_data(e);
    bool on = !ui->def->is_on();
    const char *problem = ui->def->set_on(on);
    ESP_LOGI(TAG, "%s -> %s%s%s", ui->def->label, on ? "on" : "off",
             problem ? ": " : "", problem ? problem : "");
    if (problem) {
        toast_warn(problem);
    }
    show_all();
}

static void level_set(level_ui_t *ui, bool save)
{
    uint8_t level = (uint8_t)lv_slider_get_value(ui->slider);
    const char *problem = ui->def->set(level, save);
    if (save) {
        ESP_LOGI(TAG, "%s -> %u%%%s%s", ui->def->label, (unsigned)level,
                 problem ? ": " : "", problem ? problem : "");
        if (problem) {
            toast_warn(problem);
        }
        show_all();
    }
}

/* While dragged, the level follows the finger and is not saved; let go, or
 * moved by a key, it is saved. */
static void level_changed_cb(lv_event_t *e)
{
    level_ui_t *ui = lv_event_get_user_data(e);
    level_set(ui, !lv_slider_is_dragged(ui->slider));
}

static void level_released_cb(lv_event_t *e)
{
    level_set(lv_event_get_user_data(e), true);
}

static void backdrop_clicked_cb(lv_event_t *e)
{
    /* Only a tap on the backdrop itself, not one that bubbled up from the
     * panel. */
    if (lv_event_get_target(e) == s_backdrop) {
        quick_panel_close();
    }
}

static void close_clicked_cb(lv_event_t *e)
{
    (void)e;
    quick_panel_close();
}

static void settings_clicked_cb(lv_event_t *e)
{
    (void)e;
    quick_panel_close();
    app_manager_launch(SETTINGS_APP_ID);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                              */
/* ------------------------------------------------------------------ */

static void styles_init(void)
{
    const theme_colors_t *tc = theme_get_colors();

    lv_style_reset(&s_style_box);
    lv_style_init(&s_style_box);
    lv_style_set_bg_opa(&s_style_box, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_style_box, 0);
    lv_style_set_pad_all(&s_style_box, 0);
    lv_style_set_pad_row(&s_style_box, GAP);
    lv_style_set_pad_column(&s_style_box, GAP);

    lv_style_reset(&s_style_panel);
    lv_style_init(&s_style_panel);
    lv_style_set_bg_color(&s_style_panel, tc->bg);
    lv_style_set_bg_opa(&s_style_panel, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_panel, tc->text);
    lv_style_set_border_width(&s_style_panel, 2);
    lv_style_set_border_side(&s_style_panel, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_pad_all(&s_style_panel, PANEL_PAD);
    lv_style_set_pad_row(&s_style_panel, GAP);
    lv_style_set_text_color(&s_style_panel, tc->text);

    lv_style_reset(&s_style_tile);
    lv_style_init(&s_style_tile);
    lv_style_set_bg_color(&s_style_tile, tc->bg);
    lv_style_set_bg_opa(&s_style_tile, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_tile, tc->text);
    lv_style_set_border_width(&s_style_tile, 1);
    lv_style_set_radius(&s_style_tile, 6);
    lv_style_set_pad_all(&s_style_tile, 2);
    lv_style_set_pad_row(&s_style_tile, 0);
    lv_style_set_text_color(&s_style_tile, tc->text);

    lv_style_reset(&s_style_tile_on);
    lv_style_init(&s_style_tile_on);
    lv_style_set_bg_color(&s_style_tile_on, tc->text);
    lv_style_set_text_color(&s_style_tile_on, tc->bg);

    lv_style_reset(&s_style_tile_pressed);
    lv_style_init(&s_style_tile_pressed);
    lv_style_set_bg_color(&s_style_tile_pressed, tc->primary);
    lv_style_set_text_color(&s_style_tile_pressed, tc->bg);

    /* The keyboard moves a thick border; with no colour to spare on e-paper,
     * that is what shows where Enter goes. */
    lv_style_reset(&s_style_focus);
    lv_style_init(&s_style_focus);
    lv_style_set_border_width(&s_style_focus, 3);
    lv_style_set_border_color(&s_style_focus, tc->text);

    lv_style_reset(&s_style_symbol);
    lv_style_init(&s_style_symbol);
    lv_style_set_text_font(&s_style_symbol, &lv_font_montserrat_18);

    lv_style_reset(&s_style_text);
    lv_style_init(&s_style_text);
    lv_style_set_text_font(&s_style_text, &lv_font_montserrat_14);

    /* Its width is the tiles', set once they are measured. */
    lv_style_reset(&s_style_tile_label);
    lv_style_init(&s_style_tile_label);
    lv_style_set_text_font(&s_style_tile_label, &lv_font_montserrat_14);
    lv_style_set_text_align(&s_style_tile_label, LV_TEXT_ALIGN_CENTER);

    lv_style_reset(&s_style_slider);
    lv_style_init(&s_style_slider);
    lv_style_set_bg_color(&s_style_slider, tc->bg);
    lv_style_set_bg_opa(&s_style_slider, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_slider, tc->text);
    lv_style_set_border_width(&s_style_slider, 1);
    lv_style_set_radius(&s_style_slider, LV_RADIUS_CIRCLE);

    lv_style_reset(&s_style_slider_ind);
    lv_style_init(&s_style_slider_ind);
    lv_style_set_bg_color(&s_style_slider_ind, tc->text);
    lv_style_set_bg_opa(&s_style_slider_ind, LV_OPA_COVER);
    lv_style_set_radius(&s_style_slider_ind, LV_RADIUS_CIRCLE);

    lv_style_reset(&s_style_slider_knob);
    lv_style_init(&s_style_slider_knob);
    lv_style_set_bg_color(&s_style_slider_knob, tc->text);
    lv_style_set_bg_opa(&s_style_slider_knob, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_slider_knob, tc->bg);
    lv_style_set_border_width(&s_style_slider_knob, 2);
    lv_style_set_radius(&s_style_slider_knob, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&s_style_slider_knob, 5);
}

static lv_obj_t *add_box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &s_style_box, LV_PART_MAIN);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(box, flow);
    return box;
}

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, lv_style_t *style)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_obj_add_style(label, style, LV_PART_MAIN);
    lv_label_set_text(label, text);
    return label;
}

/* A tile, or a footer button: the same look, either size. */
static lv_obj_t *add_button(lv_obj_t *parent, int32_t w, int32_t h, lv_event_cb_t cb,
                            void *user_data)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &s_style_tile, LV_PART_MAIN);
    lv_obj_add_style(btn, &s_style_tile_on, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_style(btn, &s_style_tile_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(btn, &s_style_focus, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_size(btn, w, h);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_group_add_obj(s_group, btn);
    return btn;
}

static void add_tiles(lv_obj_t *panel, int32_t content_w)
{
    int cols = (int)((content_w + GAP) / (TILE_MIN_W + GAP));
    if (cols < 1) cols = 1;
    int32_t tile_w = (content_w - (cols - 1) * GAP) / cols;
    lv_style_set_width(&s_style_tile_label, tile_w - 4);

    lv_obj_t *row = add_box(panel, LV_FLEX_FLOW_ROW_WRAP);
    for (int i = 0; i < s_toggle_count; i++) {
        const quick_toggle_t *def = s_toggles[i];
        if (def->available && !def->available()) {
            continue;
        }
        toggle_ui_t *ui = &s_toggle_ui[s_toggle_ui_count++];
        ui->def  = def;
        ui->tile = add_button(row, tile_w, TILE_H, toggle_clicked_cb, ui);
        if (def->symbol) {
            add_label(ui->tile, def->symbol, &s_style_symbol);
        }
        lv_obj_t *label = add_label(ui->tile, def->label, &s_style_tile_label);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        show_toggle(ui);
    }
    if (s_toggle_ui_count == 0) {
        lv_obj_delete(row);
    }
}

static void add_levels(lv_obj_t *panel)
{
    for (int i = 0; i < s_level_count; i++) {
        const quick_level_t *def = s_levels[i];
        if (def->available && !def->available()) {
            continue;
        }
        level_ui_t *ui = &s_level_ui[s_level_ui_count++];
        ui->def = def;

        lv_obj_t *row = add_box(panel, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        /* Room for the knob, which stands out of the bar at either end. */
        lv_obj_set_style_pad_ver(row, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_right(row, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);

        lv_obj_t *symbol = add_label(row, def->symbol ? def->symbol : "", &s_style_symbol);
        lv_obj_set_width(symbol, SYMBOL_W);

        lv_obj_t *slider = lv_slider_create(row);
        lv_obj_remove_style_all(slider);
        lv_obj_add_style(slider, &s_style_slider, LV_PART_MAIN);
        lv_obj_add_style(slider, &s_style_focus, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
        lv_obj_add_style(slider, &s_style_slider_ind, LV_PART_INDICATOR);
        lv_obj_add_style(slider, &s_style_slider_knob, LV_PART_KNOB);
        lv_obj_set_height(slider, SLIDER_H);
        lv_obj_set_flex_grow(slider, 1);
        /* A thin bar is hard to hit with a finger. */
        lv_obj_set_ext_click_area(slider, 12);
        lv_slider_set_range(slider, def->min ? def->min() : 0, 100);
        lv_slider_set_value(slider, def->get(), LV_ANIM_OFF);
        lv_obj_add_event_cb(slider, level_changed_cb, LV_EVENT_VALUE_CHANGED, ui);
        lv_obj_add_event_cb(slider, level_released_cb, LV_EVENT_RELEASED, ui);
        lv_group_add_obj(s_group, slider);
        ui->slider = slider;
    }
}

static void add_footer(lv_obj_t *panel)
{
    lv_obj_t *row = add_box(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *settings = add_button(row, LV_SIZE_CONTENT, FOOTER_H, settings_clicked_cb, NULL);
    lv_obj_set_style_pad_hor(settings, 10, LV_PART_MAIN);
    add_label(settings, LV_SYMBOL_SETTINGS " Settings", &s_style_text);

    lv_obj_t *close = add_button(row, LV_SIZE_CONTENT, FOOTER_H, close_clicked_cb, NULL);
    lv_obj_set_style_pad_hor(close, 16, LV_PART_MAIN);
    add_label(close, LV_SYMBOL_UP, &s_style_text);
}

static lv_indev_t *find_keypad(void)
{
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            return indev;
        }
    }
    return NULL;
}

void quick_panel_open(void)
{
    lv_obj_t *screen = ui_manager_get_screen();
    if (s_backdrop || !screen || power_ui_sleep_screen_shown()) {
        return;
    }
    power_ui_close_menu();
    styles_init();

    s_group = lv_group_create();
    s_keypad = find_keypad();
    if (s_keypad) {
        s_keypad_group_before = lv_indev_get_group(s_keypad);
        lv_indev_set_group(s_keypad, s_group);
    }

    int32_t screen_w = lv_obj_get_width(screen);
    int32_t screen_h = lv_obj_get_height(screen);
    int32_t bar_h = statusbar_get_height();

    s_backdrop = lv_obj_create(screen);
    lv_obj_remove_style_all(s_backdrop);
    lv_obj_set_pos(s_backdrop, 0, 0);
    lv_obj_set_size(s_backdrop, screen_w, screen_h);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_backdrop, backdrop_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(s_backdrop);
    lv_obj_remove_style_all(panel);
    lv_obj_add_style(panel, &s_style_panel, LV_PART_MAIN);
    lv_obj_set_pos(panel, 0, bar_h);
    lv_obj_set_size(panel, screen_w, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, screen_h - bar_h - CLOSE_STRIP, LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);  /* a tap between tiles stays in */

    s_toggle_ui_count = 0;
    s_level_ui_count = 0;
    add_tiles(panel, screen_w - 2 * PANEL_PAD);
    add_levels(panel);
    add_footer(panel);

    lv_obj_move_foreground(s_backdrop);
    s_refresh_timer = lv_timer_create(refresh_timer_cb, REFRESH_MS, NULL);
    s_open = true;
    ESP_LOGI(TAG, "open: %d switches, %d sliders", s_toggle_ui_count, s_level_ui_count);
}

bool quick_panel_close(void)
{
    if (!s_backdrop) {
        return false;
    }
    s_open = false;
    if (s_refresh_timer) {
        lv_timer_delete(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    if (s_keypad) {
        lv_indev_set_group(s_keypad, s_keypad_group_before);
        s_keypad = NULL;
        s_keypad_group_before = NULL;
    }
    /* Asynchronously: this may be running in the event of a button on the
     * panel, which must not be deleted under its own feet. */
    lv_obj_delete_async(s_backdrop);
    s_backdrop = NULL;
    s_toggle_ui_count = 0;
    s_level_ui_count = 0;
    /* The panel's objects leave the group as they are deleted; the group goes
     * once they have. */
    lv_group_t *group = s_group;
    s_group = NULL;
    lv_async_call((lv_async_cb_t)lv_group_delete, group);
    ESP_LOGI(TAG, "closed");
    return true;
}

void quick_panel_toggle(void)
{
    if (!quick_panel_close()) {
        quick_panel_open();
    }
}
