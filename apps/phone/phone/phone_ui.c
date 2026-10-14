/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Phone app's frame
 *
 * A tab bar along the bottom, as on a phone: Favourites, Recents, Contacts,
 * Keypad, each a picture over a word, the chosen one inverted (e-paper has
 * black and white only, so state is shown by inversion, never colour). Above
 * it, one page: a tab's own, or a contact's opened from one. A page is
 * deleted when left and built again when it comes back, so only one page's
 * widgets are ever in LVGL's pool; the lists keep their place meanwhile
 * (phone_lists.c).
 *
 * The app opens on the tab last open, Contacts the first time, or on
 * Recents when calls were missed. Back goes from a contact's page to its tab,
 * and from a tab leaves the app.
 */
#include "phone/phone_app.h"
#include "phone/phone_text.h"
#include "phone/phone_ui.h"
#include "reader/arabic_shape.h"

#include "hal/board.h"
#include "thistle/app_manager.h"
#include "thistle/call.h"
#include "thistle/net_manager.h"
#include "thistle/phonebook.h"
#include "thistle/sms.h"
#include "ui/manager.h"
#include "ui/theme.h"
#include "ui/toast.h"

#include "esp_log.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "phone";

LV_FONT_DECLARE(reader_font_16);
LV_FONT_DECLARE(reader_font_16_mono);
LV_FONT_DECLARE(phone_symbols_16);
LV_FONT_DECLARE(phone_symbols_16_mono);
LV_FONT_DECLARE(phone_tab_12);
LV_FONT_DECLARE(phone_tab_12_mono);
LV_FONT_DECLARE(phone_icons_18);
LV_FONT_DECLARE(phone_icons_18_mono);

#define TICK_MS       1000
#define TAB_TALL_H    40    /* a picture over a word */
#define TAB_SHORT_H   26    /* a picture alone, on a screen too short for both */
#define TALL_TABS_MIN 180   /* the app's height from which the words fit */
#define MESSENGER_ID  "com.thistle.messenger"

typedef enum { VIEW_TAB, VIEW_CONTACT } view_t;

static struct {
    lv_obj_t   *root;
    lv_obj_t   *page;               /* the page shown */
    lv_obj_t   *tabs[PHONE_TAB_COUNT];
    phone_tab_t tab;                /* the tab chosen */
    view_t      view;
    lv_timer_t *tick;
    int32_t     w, h, page_h, tab_h;
    bool        epaper, calls, messages;
} s_ui;

/* The tab last open, kept while the device runs, as a phone keeps it. */
static phone_tab_t s_last_tab = PHONE_TAB_CONTACTS;

static phone_styles_t s_styles;
static struct {
    lv_style_t bar;     /* the tab bar: a rule above */
    lv_style_t tab;
    lv_style_t tab_on;  /* the chosen tab, and one pressed: inverted */
    lv_style_t word;
    lv_style_t icon;
} s_tab_styles;
static bool s_styles_made;

static lv_font_t s_text_font, s_symbol_font, s_tab_font, s_icon_font;

static const char *const TAB_NAMES[PHONE_TAB_COUNT] = {"favourites", "recents", "contacts", "keypad"};
static const char *const TAB_WORDS[PHONE_TAB_COUNT] = {"Favourites", "Recents", "Contacts", "Keypad"};
static const char *const TAB_ICONS[PHONE_TAB_COUNT] = {
    PHONE_ICON_STAR, PHONE_ICON_CLOCK, PHONE_ICON_PERSON, PHONE_ICON_KEYPAD,
};

/* ------------------------------------------------------------------ */
/* Fonts and styles                                                     */
/* ------------------------------------------------------------------ */

static bool display_is_epaper(void)
{
    const hal_registry_t *reg = hal_get_registry();
    return reg && reg->display && reg->display->type == HAL_DISPLAY_TYPE_EPAPER;
}

static void fonts_init(void)
{
    bool mono = s_ui.epaper;
    s_symbol_font = mono ? phone_symbols_16_mono : phone_symbols_16;
    s_symbol_font.fallback = &lv_font_montserrat_14;
    s_text_font = mono ? reader_font_16_mono : reader_font_16;
    s_text_font.fallback = &s_symbol_font;
    s_tab_font = mono ? phone_tab_12_mono : phone_tab_12;
    s_tab_font.fallback = &lv_font_montserrat_14;
    s_icon_font = mono ? phone_icons_18_mono : phone_icons_18;
    s_icon_font.fallback = &lv_font_montserrat_14;
}

static lv_style_t *all_styles(size_t *count)
{
    /* Both structs are nothing but styles. */
    *count = sizeof s_styles / sizeof(lv_style_t);
    return (lv_style_t *)&s_styles;
}

static void styles_reset(void)
{
    if (!s_styles_made) return;
    size_t n;
    lv_style_t *s = all_styles(&n);
    for (size_t i = 0; i < n; i++) lv_style_reset(&s[i]);
    s = (lv_style_t *)&s_tab_styles;
    for (size_t i = 0; i < sizeof s_tab_styles / sizeof(lv_style_t); i++) lv_style_reset(&s[i]);
    s_styles_made = false;
}

static void styles_init(void)
{
    styles_reset();
    size_t n;
    lv_style_t *all = all_styles(&n);
    for (size_t i = 0; i < n; i++) lv_style_init(&all[i]);
    all = (lv_style_t *)&s_tab_styles;
    for (size_t i = 0; i < sizeof s_tab_styles / sizeof(lv_style_t); i++) lv_style_init(&all[i]);
    s_styles_made = true;

    const theme_colors_t *clr = theme_get_colors();
    phone_styles_t *s = &s_styles;

    lv_style_set_bg_opa(&s->box, LV_OPA_TRANSP);
    lv_style_set_border_width(&s->box, 0);
    lv_style_set_pad_all(&s->box, 0);
    lv_style_set_pad_row(&s->box, 0);
    lv_style_set_pad_column(&s->box, 0);
    lv_style_set_radius(&s->box, 0);

    lv_style_set_bg_color(&s->page, clr->bg);
    lv_style_set_bg_opa(&s->page, LV_OPA_COVER);
    lv_style_set_border_width(&s->page, 0);
    lv_style_set_pad_all(&s->page, 0);
    lv_style_set_radius(&s->page, 0);
    lv_style_set_text_color(&s->page, clr->text);

    lv_style_set_bg_color(&s->bar, clr->surface);
    lv_style_set_bg_opa(&s->bar, LV_OPA_COVER);
    lv_style_set_border_side(&s->bar, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&s->bar, clr->text);
    lv_style_set_border_width(&s->bar, 1);
    lv_style_set_pad_hor(&s->bar, PHONE_PAD);
    lv_style_set_pad_ver(&s->bar, 0);
    lv_style_set_radius(&s->bar, 0);
    lv_style_set_text_color(&s->bar, clr->text);

    lv_style_set_bg_color(&s->button, clr->bg);
    lv_style_set_bg_opa(&s->button, LV_OPA_COVER);
    lv_style_set_border_color(&s->button, clr->text);
    lv_style_set_border_width(&s->button, 1);
    lv_style_set_radius(&s->button, 4);
    lv_style_set_pad_hor(&s->button, 6);
    lv_style_set_pad_ver(&s->button, 4);
    lv_style_set_text_color(&s->button, clr->text);

    lv_style_set_bg_color(&s->button_pressed, clr->text);
    lv_style_set_text_color(&s->button_pressed, clr->bg);

    lv_style_set_bg_color(&s->primary, clr->text);
    lv_style_set_text_color(&s->primary, clr->bg);

    lv_style_set_bg_color(&s->primary_pressed, clr->bg);
    lv_style_set_text_color(&s->primary_pressed, clr->text);

    /* With no colour to spare on e-paper, an outline shows where Enter goes. */
    lv_style_set_outline_width(&s->focus_ring, 2);
    lv_style_set_outline_color(&s->focus_ring, clr->text);
    lv_style_set_outline_pad(&s->focus_ring, 1);

    lv_style_set_text_font(&s->text, &s_text_font);
    lv_style_set_text_line_space(&s->text, -2);   /* DejaVu's line is generous */
    lv_style_set_text_font(&s->small, &lv_font_montserrat_14);
    lv_style_set_text_font(&s->medium, &lv_font_montserrat_18);
    lv_style_set_text_font(&s->large, &lv_font_montserrat_22);

    lv_style_set_text_font(&s->note, &s_text_font);
    lv_style_set_text_line_space(&s->note, -2);
    lv_style_set_text_color(&s->note, clr->text_secondary);
    lv_style_set_text_align(&s->note, LV_TEXT_ALIGN_CENTER);

    lv_style_set_bg_color(&s->field, clr->bg);
    lv_style_set_bg_opa(&s->field, LV_OPA_COVER);
    lv_style_set_border_color(&s->field, clr->text);
    lv_style_set_border_width(&s->field, 1);
    lv_style_set_radius(&s->field, 4);
    lv_style_set_pad_hor(&s->field, 6);
    lv_style_set_pad_ver(&s->field, 2);
    lv_style_set_text_color(&s->field, clr->text);
    lv_style_set_text_font(&s->field, &s_text_font);

    lv_style_set_text_color(&s->placeholder, clr->text_secondary);

    /* A bar before the next letter. It does not blink on e-paper, where each
     * blink would be a refresh. */
    lv_style_set_border_side(&s->cursor, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_width(&s->cursor, 2);
    lv_style_set_border_color(&s->cursor, clr->text);
    lv_style_set_bg_opa(&s->cursor, LV_OPA_TRANSP);
    lv_style_set_anim_duration(&s->cursor, s_ui.epaper ? 0 : 500);

    lv_style_set_bg_color(&s->table, clr->bg);
    lv_style_set_bg_opa(&s->table, LV_OPA_COVER);
    lv_style_set_border_width(&s->table, 0);
    lv_style_set_pad_all(&s->table, 0);
    lv_style_set_radius(&s->table, 0);

    lv_style_set_text_font(&s->cell, &s_text_font);
    lv_style_set_text_color(&s->cell, clr->text);
    lv_style_set_text_line_space(&s->cell, -2);
    lv_style_set_bg_color(&s->cell, clr->surface);
    lv_style_set_bg_opa(&s->cell, LV_OPA_COVER);
    lv_style_set_pad_left(&s->cell, PHONE_CELL_PAD);
    lv_style_set_pad_right(&s->cell, PHONE_CELL_PAD);
    lv_style_set_pad_top(&s->cell, 4);
    lv_style_set_pad_bottom(&s->cell, 4);

    lv_style_set_bg_color(&s->cell_chosen, clr->primary);
    lv_style_set_text_color(&s->cell_chosen, clr->bg);

    lv_style_set_bg_color(&s->scrollbar, clr->primary);
    lv_style_set_bg_opa(&s->scrollbar, LV_OPA_COVER);
    lv_style_set_width(&s->scrollbar, 2);

    lv_style_set_radius(&s->avatar, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&s->avatar, 2);
    lv_style_set_border_color(&s->avatar, clr->text);
    lv_style_set_bg_color(&s->avatar, clr->surface);
    lv_style_set_bg_opa(&s->avatar, LV_OPA_COVER);
    lv_style_set_text_color(&s->avatar, clr->text);
    lv_style_set_pad_all(&s->avatar, 0);

    lv_style_set_bg_opa(&s->grid, LV_OPA_TRANSP);
    lv_style_set_border_width(&s->grid, 0);
    lv_style_set_pad_all(&s->grid, 0);
    lv_style_set_pad_row(&s->grid, PHONE_GAP);
    lv_style_set_pad_column(&s->grid, PHONE_GAP);
    lv_style_set_radius(&s->grid, 0);

    lv_style_set_bg_opa(&s->flat, LV_OPA_TRANSP);
    lv_style_set_border_width(&s->flat, 0);
    lv_style_set_bg_opa(&s->flat_pressed, LV_OPA_COVER);

    lv_style_set_border_side(&s->rule, LV_BORDER_SIDE_TOP);
    lv_style_set_border_color(&s->rule, clr->text_secondary);
    lv_style_set_border_width(&s->rule, 1);
    lv_style_set_pad_top(&s->rule, 4);

    lv_style_set_bg_color(&s_tab_styles.bar, clr->bg);
    lv_style_set_bg_opa(&s_tab_styles.bar, LV_OPA_COVER);
    lv_style_set_border_side(&s_tab_styles.bar, LV_BORDER_SIDE_TOP);
    lv_style_set_border_color(&s_tab_styles.bar, clr->text);
    lv_style_set_border_width(&s_tab_styles.bar, 1);
    lv_style_set_pad_all(&s_tab_styles.bar, 0);
    lv_style_set_radius(&s_tab_styles.bar, 0);

    lv_style_set_bg_color(&s_tab_styles.tab, clr->bg);
    lv_style_set_bg_opa(&s_tab_styles.tab, LV_OPA_COVER);
    lv_style_set_text_color(&s_tab_styles.tab, clr->text);
    lv_style_set_border_width(&s_tab_styles.tab, 0);
    lv_style_set_pad_all(&s_tab_styles.tab, 0);
    lv_style_set_pad_row(&s_tab_styles.tab, 1);
    lv_style_set_radius(&s_tab_styles.tab, 0);

    lv_style_set_bg_color(&s_tab_styles.tab_on, clr->text);
    lv_style_set_text_color(&s_tab_styles.tab_on, clr->bg);

    lv_style_set_text_font(&s_tab_styles.word, &s_tab_font);
    lv_style_set_text_font(&s_tab_styles.icon, &s_icon_font);
}

const phone_styles_t *phone_ui_styles(void)
{
    return &s_styles;
}

const lv_font_t *phone_ui_font(void)
{
    return &s_text_font;
}

const lv_font_t *phone_ui_tab_font(void)
{
    return &s_tab_font;
}

const lv_font_t *phone_ui_icon_font(void)
{
    return &s_icon_font;
}

bool phone_ui_epaper(void)
{
    return s_ui.epaper;
}

int32_t phone_ui_width(void)
{
    return s_ui.w;
}

int32_t phone_ui_page_height(void)
{
    return s_ui.page_h;
}

bool phone_ui_calls_supported(void)
{
    return s_ui.calls;
}

bool phone_ui_messages_supported(void)
{
    return s_ui.messages;
}

/* ------------------------------------------------------------------ */
/* Widgets                                                              */
/* ------------------------------------------------------------------ */

lv_obj_t *phone_ui_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &s_styles.box, LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    return box;
}

lv_obj_t *phone_ui_label(lv_obj_t *parent, const lv_style_t *font, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    /* The theme gives every label a colour of its own, which would keep it
     * from following its parent's, as a button's label must. */
    lv_obj_remove_style_all(label);
    if (font) lv_obj_add_style(label, font, LV_PART_MAIN);
    lv_label_set_text(label, "");
    phone_ui_set_text(label, text ? text : "");
    return label;
}

lv_obj_t *phone_ui_button_label(lv_obj_t *button)
{
    return lv_obj_get_child(button, 0);
}

lv_obj_t *phone_ui_button(lv_obj_t *parent, const char *text, const lv_style_t *font, bool primary,
                          lv_event_cb_t clicked, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &s_styles.button, LV_PART_MAIN);
    lv_obj_add_style(btn, &s_styles.button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    if (primary) {
        lv_obj_add_style(btn, &s_styles.primary, LV_PART_MAIN);
        lv_obj_add_style(btn, &s_styles.primary_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    }
    lv_obj_add_style(btn, &s_styles.focus_ring, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    /* A tap does not take the keyboard from the page's field: only the
     * keys move it. */
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_t *label = phone_ui_label(btn, font ? font : &s_styles.small, text);
    lv_obj_center(label);
    if (clicked) lv_obj_add_event_cb(btn, clicked, LV_EVENT_CLICKED, user_data);
    return btn;
}

void phone_ui_flat(lv_obj_t *button)
{
    lv_obj_add_style(button, &s_styles.flat, LV_PART_MAIN);
    lv_obj_add_style(button, &s_styles.flat_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
}

lv_obj_t *phone_ui_table(lv_obj_t *page, int32_t y, int32_t height, uint32_t columns)
{
    lv_obj_t *table = lv_table_create(page);
    lv_obj_remove_style_all(table);
    lv_obj_add_style(table, &s_styles.table, LV_PART_MAIN);
    lv_obj_add_style(table, &s_styles.cell, LV_PART_ITEMS);
    /* The selected row, inverted, as a pressed one. */
    lv_obj_add_style(table, &s_styles.cell_chosen, LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_add_style(table, &s_styles.cell_chosen, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_style(table, &s_styles.scrollbar, LV_PART_SCROLLBAR);
    theme_rule_table_rows(table);
    lv_obj_set_pos(table, 0, y);
    lv_obj_set_size(table, s_ui.w, height > 1 ? height : 1);
    lv_obj_set_scrollbar_mode(table, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(table, LV_DIR_VER);
    /* A tap opens its row; it must not take the keyboard from a search. */
    lv_obj_remove_flag(table, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_table_set_column_count(table, columns);
    if (columns == 1) lv_table_set_column_width(table, 0, s_ui.w);
    return table;
}

void phone_ui_set_text(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    size_t len = strlen(text);
    char *shaped = arabic_present(text, len) ? arabic_shape(text, len, NULL, 0, NULL) : NULL;
    const char *t = shaped ? shaped : text;
    if (strcmp(lv_label_get_text(label), t) != 0) lv_label_set_text(label, t);
    free(shaped);
}

void phone_ui_fit(const char *text, const lv_font_t *font, int32_t width, char *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = '\0';
    if (!text) return;
    size_t len = strlen(text);
    char *shaped = arabic_present(text, len) ? arabic_shape(text, len, NULL, 0, &len) : NULL;
    const char *t = shaped ? shaped : text;

    if (lv_text_get_width(t, (uint32_t)len, font, 0) <= width) {
        snprintf(out, cap, "%.*s", (int)len, t);
    } else {
        int32_t room = width - lv_text_get_width(PHONE_ELLIPSIS, sizeof(PHONE_ELLIPSIS) - 1, font, 0);
        size_t end = len;
        while (end > 0 && lv_text_get_width(t, (uint32_t)end, font, 0) > room) {
            do end--; while (end > 0 && ((unsigned char)t[end] & 0xC0) == 0x80);
        }
        while (end > 0 && t[end - 1] == ' ') end--;
        if (end + sizeof(PHONE_ELLIPSIS) > cap) end = 0;  /* never cut a character in two */
        snprintf(out, cap, "%.*s" PHONE_ELLIPSIS, (int)end, t);
    }
    free(shaped);
}

void phone_ui_group_add(lv_obj_t *obj)
{
    lv_group_t *grp = lv_group_get_default();
    if (grp && obj && lv_obj_get_group(obj) != grp) lv_group_add_obj(grp, obj);
}

void phone_ui_focus(lv_obj_t *obj)
{
    lv_group_t *grp = lv_group_get_default();
    if (!grp || !obj) return;
    phone_ui_group_add(obj);
    lv_group_focus_obj(obj);
}

bool phone_ui_from_keys(void)
{
    lv_indev_t *indev = lv_indev_active();
    return indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD;
}

/* ------------------------------------------------------------------ */
/* Pages                                                                */
/* ------------------------------------------------------------------ */

phone_tab_t phone_ui_tab(void)
{
    return s_ui.tab;
}

const char *phone_ui_tab_name(phone_tab_t tab)
{
    return tab < PHONE_TAB_COUNT ? TAB_WORDS[tab] : "";
}

/* Lets the page shown go: its widgets first, then the page. */
static void release_page(void)
{
    if (!s_ui.page) return;
    if (s_ui.view == VIEW_CONTACT) phone_contact_release();
    else if (s_ui.tab == PHONE_TAB_KEYPAD) phone_keypad_release();
    else phone_lists_release();
    lv_obj_delete(s_ui.page);
    s_ui.page = NULL;
}

static lv_obj_t *new_page(void)
{
    lv_obj_t *page = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(page);
    lv_obj_add_style(page, &s_styles.page, LV_PART_MAIN);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_size(page, s_ui.w, s_ui.page_h);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_CLICKABLE);
    return page;
}

static void mark_tab(phone_tab_t tab)
{
    for (int i = 0; i < PHONE_TAB_COUNT; i++) {
        if (!s_ui.tabs[i]) continue;
        bool on = (phone_tab_t)i == tab;
        /* The e-paper redraws whatever is touched, changed or not. */
        if (lv_obj_has_state(s_ui.tabs[i], LV_STATE_CHECKED) != on) {
            if (on) lv_obj_add_state(s_ui.tabs[i], LV_STATE_CHECKED);
            else lv_obj_remove_state(s_ui.tabs[i], LV_STATE_CHECKED);
        }
    }
}

void phone_ui_show_tab(phone_tab_t tab)
{
    if (!s_ui.root || tab >= PHONE_TAB_COUNT) return;
    release_page();
    s_ui.tab = tab;
    s_ui.view = VIEW_TAB;
    s_last_tab = tab;
    mark_tab(tab);
    ESP_LOGI(TAG, "tab %s", TAB_NAMES[tab]);
    s_ui.page = new_page();
    if (tab == PHONE_TAB_KEYPAD) phone_keypad_build(s_ui.page);
    else phone_lists_build(s_ui.page, tab);
}

static void open_contact_page(void)
{
    release_page();
    s_ui.view = VIEW_CONTACT;
    s_ui.page = new_page();
    phone_contact_build(s_ui.page);
}

void phone_ui_open_contact(uint32_t index)
{
    if (!s_ui.root) return;
    if (!phone_contact_set_index(index)) {
        toast_warn("The contacts have changed");
        return;
    }
    open_contact_page();
}

void phone_ui_open_number(const char *number)
{
    if (!s_ui.root || !number || !number[0]) return;
    int32_t index = rs_phonebook_find_number(number);
    if (index < 0 || !phone_contact_set_index((uint32_t)index)) phone_contact_set_number(number);
    open_contact_page();
}

bool phone_ui_back(void)
{
    if (!s_ui.root) return false;
    if (s_ui.view == VIEW_CONTACT) {
        phone_ui_show_tab(s_ui.tab);
        return true;
    }
    return s_ui.tab != PHONE_TAB_KEYPAD && phone_lists_back();
}

static bool back_handler(void)
{
    return phone_ui_back();
}

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_ui.page) return;
    if (s_ui.view == VIEW_CONTACT) phone_contact_tick();
    else if (s_ui.tab == PHONE_TAB_KEYPAD) phone_keypad_tick();
    else phone_lists_tick();
}

/* ------------------------------------------------------------------ */
/* The tab bar                                                          */
/* ------------------------------------------------------------------ */

static void tab_clicked_cb(lv_event_t *e)
{
    phone_tab_t tab = (phone_tab_t)(uintptr_t)lv_event_get_user_data(e);
    if (tab == s_ui.tab && s_ui.view == VIEW_TAB) {
        /* The tab shown, tapped again: to the top of its list. */
        if (tab != PHONE_TAB_KEYPAD) phone_lists_top();
        return;
    }
    phone_ui_show_tab(tab);
}

static void build_tabbar(void)
{
    bool words = s_ui.tab_h >= TAB_TALL_H;
    lv_obj_t *bar = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, &s_tab_styles.bar, LV_PART_MAIN);
    lv_obj_set_pos(bar, 0, s_ui.page_h);
    lv_obj_set_size(bar, s_ui.w, s_ui.tab_h);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    int32_t x = 0;
    for (int i = 0; i < PHONE_TAB_COUNT; i++) {
        /* The last tab takes what the division leaves. */
        int32_t tw = i == PHONE_TAB_COUNT - 1 ? s_ui.w - x : s_ui.w / PHONE_TAB_COUNT;
        lv_obj_t *tab = lv_obj_create(bar);
        lv_obj_remove_style_all(tab);
        lv_obj_add_style(tab, &s_tab_styles.tab, LV_PART_MAIN);
        lv_obj_add_style(tab, &s_tab_styles.tab_on, LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_add_style(tab, &s_tab_styles.tab_on, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_add_style(tab, &s_styles.focus_ring, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
        /* Below the bar's rule, which stays drawn over the chosen tab. */
        lv_obj_set_pos(tab, x, 1);
        lv_obj_set_size(tab, tw, s_ui.tab_h - 1);
        lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(tab, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_flag(tab, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        phone_ui_label(tab, &s_tab_styles.icon, TAB_ICONS[i]);
        if (words) phone_ui_label(tab, &s_tab_styles.word, TAB_WORDS[i]);
        lv_obj_add_event_cb(tab, tab_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        phone_ui_group_add(tab);
        s_ui.tabs[i] = tab;
        x += tw;
    }
}

/* ------------------------------------------------------------------ */
/* Calling and messaging                                                */
/* ------------------------------------------------------------------ */

/* Why the modem cannot place a call now, from what the network coordinator
 * knows of it. */
static const char *why_not_now(void)
{
    if (rs_call_in_progress()) return "A call is already up";
    net_cellular_status_t st;
    if (!rs_call_supported() || net_cellular_get_status(&st) != ESP_OK || !st.present) {
        return "This device cannot make calls";
    }
    if (!st.enabled && !st.sms) return "Calls are off. Settings > Mobile data > Calls and SMS";
    switch (st.info.sim) {
    case HAL_SIM_ABSENT:
        return "No SIM card";
    case HAL_SIM_PIN_REQUIRED:
    case HAL_SIM_PUK_REQUIRED:
        return "The SIM is locked";
    case HAL_SIM_ERROR:
        return "The SIM card is not working";
    default:
        break;
    }
    if (st.info.powered && st.info.sim == HAL_SIM_READY &&
        st.info.reg != HAL_CELL_REG_HOME && st.info.reg != HAL_CELL_REG_ROAMING) {
        return "No mobile network yet";
    }
    return "The modem is starting. Try again in a moment";
}

bool phone_ui_call(const char *number)
{
    if (!number || !number[0]) return false;
    ESP_LOGI(TAG, "call %s", number);
    esp_err_t err = rs_call_dial(number);
    if (err == ESP_OK) return true;

    const char *why;
    switch (err) {
    case ESP_ERR_NOT_SUPPORTED:
        why = "This device cannot make calls";
        break;
    case ESP_ERR_INVALID_ARG:
        why = "That is not a number that can be called";
        break;
    case ESP_ERR_INVALID_STATE:
        why = why_not_now();
        break;
    default:
        why = "The call could not be placed";
        break;
    }
    ESP_LOGW(TAG, "call %s: %s", number, esp_err_to_name(err));
    toast_warn(why);
    return false;
}

void phone_ui_message(const char *number_as_written, const char *name)
{
    /* As the network writes it, so that the Messenger's conversation is the
     * one its replies land in. */
    char number[CALL_NUMBER_MAX];
    phone_text_compact(number_as_written, number, sizeof number);
    if (!number[0]) return;
    ESP_LOGI(TAG, "message %s", number);
    if (!s_ui.messages) {
        toast_warn("There is no Messenger on this device");
        return;
    }
    rs_sms_request_compose(number, name);
    /* The Messenger coming forward closes this app: nothing of it is touched
     * after a launch that worked. */
    if (app_manager_launch(MESSENGER_ID) != ESP_OK) {
        char n[SMS_SENDER_MAX], m[PB_NAME_MAX];
        rs_sms_take_compose(n, sizeof n, m, sizeof m);  /* not for a later launch */
        toast_warn("The Messenger did not open");
    }
}

static bool app_registered(const char *id)
{
    const app_manifest_t *apps[48];
    int n = app_manager_list_apps(apps, (int)(sizeof apps / sizeof apps[0]));
    for (int i = 0; i < n; i++) {
        if (apps[i] && apps[i]->id && strcmp(apps[i]->id, id) == 0) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

static void log_pool(const char *when)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "%s: LVGL pool %u KB free of %u, largest block %u KB", when,
             (unsigned)(mon.free_size / 1024), (unsigned)(mon.total_size / 1024),
             (unsigned)(mon.free_biggest_size / 1024));
}

esp_err_t phone_ui_create(lv_obj_t *parent)
{
    if (!parent) parent = lv_screen_active();
    lv_obj_update_layout(parent);
    memset(&s_ui, 0, sizeof s_ui);
    s_ui.w = lv_obj_get_width(parent);
    s_ui.h = lv_obj_get_height(parent);
    if (s_ui.w <= 0) s_ui.w = 240;
    if (s_ui.h <= 0) s_ui.h = 296;
    s_ui.epaper = display_is_epaper();
    s_ui.calls = rs_call_supported();
    s_ui.messages = s_ui.calls && app_registered(MESSENGER_ID);
    s_ui.tab_h = s_ui.h >= TALL_TABS_MIN ? TAB_TALL_H : TAB_SHORT_H;
    if (s_ui.tab_h > s_ui.h / 2) s_ui.tab_h = s_ui.h / 2;
    s_ui.page_h = s_ui.h - s_ui.tab_h;
    fonts_init();
    styles_init();

    /* The first ask starts the search of the card, if none has run. */
    (void)rs_phonebook_seq();

    s_ui.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.root);
    lv_obj_add_style(s_ui.root, &s_styles.page, LV_PART_MAIN);
    lv_obj_set_pos(s_ui.root, 0, 0);
    lv_obj_set_size(s_ui.root, s_ui.w, s_ui.h);
    lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_SCROLLABLE);
    ui_manager_set_back_handler(s_ui.root, back_handler);

    build_tabbar();
    phone_lists_forget();
    /* Missed calls: what the user opens the app to see. */
    phone_ui_show_tab(rs_call_missed_count() > 0 ? PHONE_TAB_RECENTS : s_last_tab);
    s_ui.tick = lv_timer_create(tick_cb, TICK_MS, NULL);
    log_pool("open");
    return ESP_OK;
}

void phone_ui_destroy(void)
{
    if (s_ui.tick) lv_timer_delete(s_ui.tick);
    s_ui.tick = NULL;
    release_page();
    if (s_ui.root) lv_obj_delete(s_ui.root);
    memset(&s_ui, 0, sizeof s_ui);
    /* Nothing uses them now; what they hold goes back to the pool. */
    styles_reset();
    log_pool("closed");
}
