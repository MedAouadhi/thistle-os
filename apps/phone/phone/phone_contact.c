/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — a contact's page, or a number's
 *
 * A header with Back and, for a contact, its star; then its initials in a
 * circle, its name, and each of its numbers with what kind it is, a Call
 * button and a Message button. A number that is no contact gets the same
 * page with the number as its name and no star.
 *
 * Call hands the number to the kernel, whose call screen comes up over
 * everything; Message asks the Messenger to open an SMS to it and brings
 * the Messenger forward (thistle/sms.h). Where the device has no modem that
 * makes calls there are neither, and the page is an address book's.
 */
#include "phone/phone_text.h"
#include "phone/phone_ui.h"

#include "thistle/call.h"
#include "thistle/phonebook.h"
#include "ui/toast.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "phone";

#define AVATAR_D        48
#define AVATAR_MIN_H    170   /* the page's height from which the circle fits */
#define ACTION_H        32
#define STAR_W          40

static struct {
    bool      is_contact;
    uint32_t  index;
    char      number[CALL_NUMBER_MAX];   /* a number that is no contact */
    lv_obj_t *star;
    uint32_t  book_seq;
} s_cp;

/* The contact shown, and one looked up: off the LVGL task's stack. */
static EXT_RAM_BSS_ATTR pb_contact_t s_contact;
static EXT_RAM_BSS_ATTR pb_contact_t s_found;

bool phone_contact_set_index(uint32_t index)
{
    s_cp.is_contact = rs_phonebook_get(index, &s_contact);
    s_cp.index = index;
    s_cp.number[0] = '\0';
    return s_cp.is_contact;
}

void phone_contact_set_number(const char *number)
{
    s_cp.is_contact = false;
    snprintf(s_cp.number, sizeof s_cp.number, "%s", number ? number : "");
}

/* The contact's index now: the book may have been read again since the page
 * opened, and moved it. -1 when it is gone. */
static int32_t locate(void)
{
    if (rs_phonebook_get(s_cp.index, &s_found) && strcmp(s_found.name, s_contact.name) == 0) {
        return (int32_t)s_cp.index;
    }
    uint32_t found[8];
    uint32_t n = rs_phonebook_search(s_contact.name, found, 8);
    for (uint32_t i = 0; i < n && i < 8; i++) {
        if (!rs_phonebook_get(found[i], &s_found) || strcmp(s_found.name, s_contact.name) != 0) continue;
        if (!s_contact.number_count ||
            (s_found.number_count &&
             phone_text_same_number(s_found.numbers[0].number, s_contact.numbers[0].number))) {
            return (int32_t)found[i];
        }
    }
    return -1;
}

static void show_star(void)
{
    if (s_cp.star) {
        phone_ui_set_text(phone_ui_button_label(s_cp.star), s_contact.starred ? PHONE_STAR : PHONE_STAR_HOLLOW);
    }
}

/* ------------------------------------------------------------------ */
/* Events                                                               */
/* ------------------------------------------------------------------ */

static void back_cb(lv_event_t *e)
{
    (void)e;
    phone_ui_back();
}

static void star_cb(lv_event_t *e)
{
    (void)e;
    if (!s_cp.is_contact) return;
    int32_t index = locate();
    if (index < 0) {
        toast_warn("The contacts have changed; open it again");
        return;
    }
    bool on = !s_found.starred;
    if (rs_phonebook_set_starred((uint32_t)index, on) != ESP_OK) {
        toast_warn("Could not keep the star on the SD card");
        return;
    }
    s_cp.index = (uint32_t)index;
    s_contact.starred = on;
    ESP_LOGI(TAG, "star %s %s", s_contact.name, on ? "on" : "off");
    show_star();
    toast_show(on ? "Added to favourites" : "Removed from favourites", TOAST_INFO, 1500);
}

static void call_cb(lv_event_t *e)
{
    phone_ui_call(lv_event_get_user_data(e));
}

static void message_cb(lv_event_t *e)
{
    phone_ui_message(lv_event_get_user_data(e), s_cp.is_contact ? s_contact.name : NULL);
}

/* Up, Down, Left and Right go from one button to the next: the T-Deck's
 * keyboard has no Tab. */
static void key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    lv_group_t *grp = lv_group_get_default();
    if (!grp) return;
    if (key == LV_KEY_DOWN || key == LV_KEY_RIGHT) lv_group_focus_next(grp);
    else if (key == LV_KEY_UP || key == LV_KEY_LEFT) lv_group_focus_prev(grp);
}

static void take_keys(lv_obj_t *obj)
{
    phone_ui_group_add(obj);
    lv_obj_add_event_cb(obj, key_cb, LV_EVENT_KEY, NULL);
}

/* ------------------------------------------------------------------ */
/* Building                                                             */
/* ------------------------------------------------------------------ */

/* A number: what kind it is, the number, and what can be done with it. */
static void add_number(lv_obj_t *col, const char *label, const char *number)
{
    const phone_styles_t *st = phone_ui_styles();
    lv_obj_t *block = phone_ui_box(col);
    lv_obj_add_style(block, &st->rule, LV_PART_MAIN);
    lv_obj_set_size(block, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(block, 2, LV_PART_MAIN);

    if (label && label[0]) phone_ui_label(block, &st->small, label);
    lv_obj_t *n = phone_ui_label(block, phone_text_ascii(number) ? &st->medium : &st->text, number);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(n, LV_PCT(100));

    bool calls = phone_ui_calls_supported(), messages = phone_ui_messages_supported();
    if (!calls && !messages) return;
    lv_obj_t *row = phone_ui_box(block);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, PHONE_GAP, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 2, LV_PART_MAIN);
    if (calls) {
        lv_obj_t *call = phone_ui_button(row, LV_SYMBOL_CALL "  Call", &st->small, true, call_cb, (void *)number);
        lv_obj_set_height(call, ACTION_H);
        lv_obj_set_flex_grow(call, 1);
        take_keys(call);
    }
    if (messages) {
        lv_obj_t *msg = phone_ui_button(row, LV_SYMBOL_ENVELOPE "  Message", &st->small, false, message_cb,
                                        (void *)number);
        lv_obj_set_height(msg, ACTION_H);
        lv_obj_set_flex_grow(msg, 1);
        take_keys(msg);
    }
}

/* The contact's initials in a circle, or a person for a name with none. */
static void add_avatar(lv_obj_t *col, const char *name)
{
    const phone_styles_t *st = phone_ui_styles();
    lv_obj_t *circle = phone_ui_box(col);
    lv_obj_add_style(circle, &st->avatar, LV_PART_MAIN);
    lv_obj_set_size(circle, AVATAR_D, AVATAR_D);

    char initials[12];
    phone_text_initials(name, initials, sizeof initials);
    lv_obj_t *label;
    if (!initials[0]) {
        label = phone_ui_label(circle, NULL, PHONE_ICON_PERSON);
        lv_obj_set_style_text_font(label, phone_ui_icon_font(), LV_PART_MAIN);
    } else if (phone_text_ascii(initials)) {
        label = phone_ui_label(circle, &st->large, initials);
    } else {
        /* One letter: two of Arabic would join into a word. */
        size_t n = 1;
        while (initials[n] && ((unsigned char)initials[n] & 0xC0) == 0x80) n++;
        initials[n] = '\0';
        label = phone_ui_label(circle, &st->text, initials);
    }
    lv_obj_center(label);
}

void phone_contact_build(lv_obj_t *page)
{
    const phone_styles_t *st = phone_ui_styles();
    int32_t w = phone_ui_width();
    int32_t h = phone_ui_page_height();
    s_cp.star = NULL;
    s_cp.book_seq = rs_phonebook_seq();
    const char *title = s_cp.is_contact ? s_contact.name : s_cp.number;

    /* The header: Back to the tab it came from, and the star. */
    int32_t bar_h = PHONE_BAR_H < h / 3 ? PHONE_BAR_H : h / 3;
    lv_obj_t *bar = phone_ui_box(page);
    lv_obj_add_style(bar, &st->bar, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 2, LV_PART_MAIN);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, w, bar_h);
    char back[48];
    snprintf(back, sizeof back, LV_SYMBOL_LEFT "  %s", phone_ui_tab_name(phone_ui_tab()));
    lv_obj_t *back_btn = phone_ui_button(bar, back, &st->small, false, back_cb, NULL);
    phone_ui_flat(back_btn);
    lv_obj_set_height(back_btn, bar_h - 4);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 0, 0);
    if (s_cp.is_contact) {
        s_cp.star = phone_ui_button(bar, "", &st->text, false, star_cb, NULL);
        phone_ui_flat(s_cp.star);
        lv_obj_set_size(s_cp.star, STAR_W, bar_h - 4);
        lv_obj_align(s_cp.star, LV_ALIGN_RIGHT_MID, 0, 0);
        show_star();
    }

    /* The rest, in a column that scrolls when the numbers are many. */
    lv_obj_t *col = lv_obj_create(page);
    lv_obj_remove_style_all(col);
    lv_obj_add_style(col, &st->box, LV_PART_MAIN);
    lv_obj_add_style(col, &st->scrollbar, LV_PART_SCROLLBAR);
    lv_obj_set_pos(col, 0, bar_h);
    lv_obj_set_size(col, w, h - bar_h);
    lv_obj_set_style_pad_all(col, PHONE_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(col, PHONE_PAD, LV_PART_MAIN);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_AUTO);
    /* The column has the keyboard first, so that the Enter that opened the
     * page, let go here, presses nothing; Down goes on to the first Call. */
    take_keys(col);

    if (h - bar_h >= AVATAR_MIN_H) add_avatar(col, s_cp.is_contact ? s_contact.name : "");
    lv_obj_t *name = phone_ui_label(col, phone_text_ascii(title) ? &st->large : &st->text, title);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    if (s_cp.is_contact) {
        for (uint8_t i = 0; i < s_contact.number_count && i < PB_NUMBERS_MAX; i++) {
            add_number(col, s_contact.numbers[i].label, s_contact.numbers[i].number);
        }
        if (!s_contact.number_count) {
            lv_obj_t *none = phone_ui_label(col, &st->note, "No phone number");
            lv_obj_set_width(none, LV_PCT(100));
        }
    } else {
        lv_obj_t *sub = phone_ui_label(col, &st->small, "Not in your contacts");
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        add_number(col, NULL, s_cp.number);
    }

    take_keys(back_btn);
    if (s_cp.star) take_keys(s_cp.star);
    phone_ui_focus(col);
    ESP_LOGI(TAG, "page %s", title);
}

void phone_contact_tick(void)
{
    if (!s_cp.is_contact || rs_phonebook_seq() == s_cp.book_seq) return;
    s_cp.book_seq = rs_phonebook_seq();
    int32_t index = locate();
    if (index < 0 || s_found.starred == s_contact.starred) return;
    s_cp.index = (uint32_t)index;
    s_contact.starred = s_found.starred;
    show_star();
}

void phone_contact_release(void)
{
    s_cp.star = NULL;
}
