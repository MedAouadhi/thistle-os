/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Phone app's keypad
 *
 * The number being typed, large, with the name of the contact it belongs to
 * under it; 1-9, * 0 #; a delete key (held: clears) and a Call button. A
 * long press on 0 types "+". The keyboard types into it as well: digits,
 * '+', '*' and '#', Backspace, and Enter to call. Call with nothing typed
 * brings back the number last called, as phones do.
 *
 * Upright on a screen taller than wide; on a wide one the keys go to the
 * right and the number, the name and Call to the left. The keys are one
 * lv_buttonmatrix: one widget, not twelve.
 */
#include "phone/phone_text.h"
#include "phone/phone_ui.h"

#include "thistle/call.h"
#include "thistle/phonebook.h"
#include "ui/toast.h"

#include "esp_log.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define DISPLAY_H   32
#define NAME_H      24
#define DELETE_W    48
#define ZERO_KEY    10    /* the 0 in KEYS */

static const char *const KEYS[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    "*", "0", "#", "",
};
static const char KEY_CHARS[] = "123456789*0#";

/* The number typed, kept while the device runs: leaving the app to look
 * something up does not lose it. A call placed clears it. */
static char s_number[CALL_NUMBER_MAX];

static struct {
    lv_obj_t *display;    /* the number's box, which takes the keyboard */
    lv_obj_t *number;
    lv_obj_t *name;
    lv_obj_t *grid;
    const lv_style_t *number_font;
    int32_t   number_w;
    int32_t   name_w;
    uint32_t  book_seq;
    bool      long_zero;  /* a long press on 0 typed "+": its release types nothing */
    bool      hint;       /* the name line may say "Hold 0 for +" */
} s_kp;

/* ------------------------------------------------------------------ */
/* The number                                                           */
/* ------------------------------------------------------------------ */

static void show_number(void)
{
    if (!s_kp.number) return;
    const phone_styles_t *st = phone_ui_styles();
    const lv_style_t *styles[] = {&st->large, &st->medium, &st->small};
    const lv_font_t *fonts[] = {&lv_font_montserrat_22, &lv_font_montserrat_18, &lv_font_montserrat_14};

    /* As large as fits; past that, its end, which is what is being typed. */
    int pick = -1;
    for (int i = 0; i < 3 && pick < 0; i++) {
        if (lv_text_get_width(s_number, (uint32_t)strlen(s_number), fonts[i], 0) <= s_kp.number_w) pick = i;
    }
    const char *from = s_number;
    if (pick < 0) {
        pick = 2;
        int32_t room = s_kp.number_w - lv_text_get_width(PHONE_ELLIPSIS, sizeof(PHONE_ELLIPSIS) - 1, fonts[pick], 0);
        /* The number is ASCII: a byte is a character. */
        while (*from && lv_text_get_width(from, (uint32_t)strlen(from), fonts[pick], 0) > room) from++;
    }
    char shown[CALL_NUMBER_MAX + 4];
    snprintf(shown, sizeof shown, "%s%s", from != s_number ? PHONE_ELLIPSIS : "", from);
    if (s_kp.number_font != styles[pick]) {
        if (s_kp.number_font) lv_obj_remove_style(s_kp.number, s_kp.number_font, LV_PART_MAIN);
        lv_obj_add_style(s_kp.number, styles[pick], LV_PART_MAIN);
        s_kp.number_font = styles[pick];
    }
    phone_ui_set_text(s_kp.number, shown);

    if (!s_kp.name) return;
    char name[PB_NAME_MAX] = "";
    if (s_number[0]) {
        rs_phonebook_name_for(s_number, name, sizeof name);
    } else if (s_kp.hint) {
        snprintf(name, sizeof name, "Hold 0 for +");
    }
    char fitted[PB_NAME_MAX + 4];
    phone_ui_fit(name, phone_ui_font(), s_kp.name_w, fitted, sizeof fitted);
    phone_ui_set_text(s_kp.name, fitted);
    s_kp.book_seq = rs_phonebook_seq();
}

static void type_char(char c)
{
    if (phone_text_dial_append(s_number, sizeof s_number, c)) show_number();
}

static void delete_char(void)
{
    if (phone_text_dial_delete(s_number)) show_number();
}

static void clear_number(void)
{
    if (!s_number[0]) return;
    s_number[0] = '\0';
    show_number();
}

/* The number last called, from the call log, into the field. */
static bool recall_last(void)
{
    uint32_t n = rs_call_log_count();
    call_log_entry_t entry;
    for (uint32_t i = 0; i < n; i++) {
        if (rs_call_log_get(i, &entry) && entry.kind == CALL_LOG_OUTGOING && entry.number[0]) {
            snprintf(s_number, sizeof s_number, "%s", entry.number);
            show_number();
            return true;
        }
    }
    return false;
}

static void call_number(void)
{
    if (!s_number[0]) {
        if (!recall_last()) toast_info("Type a number to call");
        return;
    }
    if (phone_ui_call(s_number)) clear_number();
}

/* ------------------------------------------------------------------ */
/* Events                                                               */
/* ------------------------------------------------------------------ */

static void grid_changed_cb(lv_event_t *e)
{
    uint32_t *id = lv_event_get_param(e);
    if (!id || *id >= sizeof KEY_CHARS - 1) return;
    if (s_kp.long_zero) {
        s_kp.long_zero = false;
        return;
    }
    type_char(KEY_CHARS[*id]);
}

static void grid_long_cb(lv_event_t *e)
{
    lv_obj_t *grid = lv_event_get_target(e);
    if (lv_buttonmatrix_get_selected_button(grid) != ZERO_KEY) return;
    s_kp.long_zero = true;
    type_char('+');
}

static void delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) clear_number();
    else delete_char();
}

static void call_cb(lv_event_t *e)
{
    (void)e;
    call_number();
}

/* The name under the number, tapped: its contact's page. */
static void name_cb(lv_event_t *e)
{
    (void)e;
    if (s_number[0] && rs_phonebook_find_number(s_number) >= 0) phone_ui_open_number(s_number);
}

static void key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    char c = phone_text_dial_char(key);
    if (c) type_char(c);
    else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) delete_char();
    else if (key == LV_KEY_ENTER) call_number();
}

/* ------------------------------------------------------------------ */
/* Building                                                             */
/* ------------------------------------------------------------------ */

static lv_obj_t *make_grid(lv_obj_t *page, int32_t x, int32_t y, int32_t w, int32_t h)
{
    const phone_styles_t *st = phone_ui_styles();
    lv_obj_t *grid = lv_buttonmatrix_create(page);
    lv_obj_remove_style_all(grid);
    lv_obj_add_style(grid, &st->grid, LV_PART_MAIN);
    lv_obj_add_style(grid, &st->button, LV_PART_ITEMS);
    lv_obj_add_style(grid, &st->button_pressed, LV_PART_ITEMS | LV_STATE_PRESSED);
    int32_t key_h = (h - 3 * PHONE_GAP) / 4;
    lv_obj_add_style(grid, key_h >= 30 ? &st->large : key_h >= 22 ? &st->medium : &st->small, LV_PART_ITEMS);
    lv_buttonmatrix_set_map(grid, KEYS);
    /* Typed when let go, so that a long press on 0 can be "+" instead; and
     * once, however long it is held. */
    lv_buttonmatrix_set_button_ctrl_all(grid, LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    lv_obj_set_pos(grid, x, y);
    lv_obj_set_size(grid, w, h > 4 ? h : 4);
    /* Touch only: the keyboard types into the number's box, so the keys are
     * out of the keyboard's group, which LVGL puts a button matrix in. */
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_remove_obj(grid);
    lv_obj_add_event_cb(grid, grid_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(grid, grid_long_cb, LV_EVENT_LONG_PRESSED, NULL);
    return grid;
}

/* Call, as wide as given: with its word where that fits. */
static void make_call(lv_obj_t *page, int32_t x, int32_t y, int32_t w, int32_t h)
{
    const phone_styles_t *st = phone_ui_styles();
    static const char WORDED[] = LV_SYMBOL_CALL "  Call";
    const lv_style_t *font = &st->small;
    const char *text = LV_SYMBOL_CALL;
    if (h >= 34 && lv_text_get_width(WORDED, sizeof WORDED - 1, &lv_font_montserrat_18, 0) + 16 <= w) {
        font = &st->medium;
        text = WORDED;
    } else if (lv_text_get_width(WORDED, sizeof WORDED - 1, &lv_font_montserrat_14, 0) + 16 <= w) {
        text = WORDED;
    }
    lv_obj_t *call = phone_ui_button(page, text, font, true, call_cb, NULL);
    lv_obj_set_pos(call, x, y);
    lv_obj_set_size(call, w, h);
    lv_group_remove_obj(call);   /* Enter calls */
}

/* Delete: a tap takes the last character, a long press all of them. */
static void make_delete(lv_obj_t *page, int32_t x, int32_t y, int32_t w, int32_t h)
{
    const phone_styles_t *st = phone_ui_styles();
    lv_obj_t *del = phone_ui_button(page, LV_SYMBOL_BACKSPACE, h >= 34 ? &st->medium : &st->small, false,
                                    NULL, NULL);
    lv_obj_set_pos(del, x, y);
    lv_obj_set_size(del, w, h);
    lv_group_remove_obj(del);    /* Backspace deletes */
    lv_obj_add_event_cb(del, delete_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(del, delete_cb, LV_EVENT_LONG_PRESSED, NULL);
}

/* The number's box, `w` wide from x, y, and when `name_w` is not 0 the
 * line under it for the name, that wide. Returns the y below them. */
static int32_t make_display(lv_obj_t *page, int32_t x, int32_t y, int32_t w, int32_t name_w)
{
    const phone_styles_t *st = phone_ui_styles();
    lv_obj_t *display = phone_ui_box(page);
    lv_obj_set_pos(display, x, y);
    lv_obj_set_size(display, w, DISPLAY_H);
    lv_obj_add_style(display, &st->rule, LV_PART_MAIN);
    lv_obj_set_style_border_side(display, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_add_event_cb(display, key_cb, LV_EVENT_KEY, NULL);
    s_kp.display = display;

    s_kp.number = phone_ui_label(display, NULL, "");
    lv_label_set_long_mode(s_kp.number, LV_LABEL_LONG_CLIP);
    lv_obj_center(s_kp.number);
    s_kp.number_w = w - 4;
    y += DISPLAY_H;

    if (name_w > 0) {
        s_kp.name = phone_ui_label(page, &st->text, "");
        lv_label_set_long_mode(s_kp.name, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(s_kp.name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(s_kp.name, x, y + 2);
        lv_obj_set_width(s_kp.name, name_w);
        s_kp.name_w = name_w;
        lv_obj_add_flag(s_kp.name, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_kp.name, name_cb, LV_EVENT_CLICKED, NULL);
        y += NAME_H;
    }
    return y;
}

static void build_unsupported(lv_obj_t *page)
{
    lv_obj_t *box = phone_ui_box(page);
    lv_obj_set_size(box, phone_ui_width(), phone_ui_page_height());
    lv_obj_set_style_pad_all(box, PHONE_PAD * 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *note = phone_ui_label(box, &phone_ui_styles()->note,
                                    "This device has no mobile network, so it cannot make calls.");
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, LV_PCT(100));
}

void phone_keypad_build(lv_obj_t *page)
{
    memset(&s_kp, 0, sizeof s_kp);
    if (!phone_ui_calls_supported()) {
        build_unsupported(page);
        return;
    }
    int32_t w = phone_ui_width();
    int32_t h = phone_ui_page_height();

    if (h < 3 * DISPLAY_H) {
        /* No room for keys (the Cardputer, which has no touch anyway): the
         * number, typed on the keyboard, with Delete beside it, and Call. */
        int32_t inner = w - 2 * PHONE_PAD;
        int32_t btn_h = h - DISPLAY_H - PHONE_PAD;
        if (btn_h > 36) btn_h = 36;
        if (btn_h < 8) btn_h = 8;
        int32_t btn_y = h - PHONE_PAD / 2 - btn_h;
        bool name = PHONE_PAD / 2 + DISPLAY_H + NAME_H <= btn_y;
        make_display(page, PHONE_PAD, PHONE_PAD / 2, inner - DELETE_W - PHONE_GAP, name ? inner : 0);
        make_delete(page, PHONE_PAD + inner - DELETE_W, PHONE_PAD / 2, DELETE_W, DISPLAY_H);
        make_call(page, PHONE_PAD, btn_y, inner, btn_h);
    } else if (h >= w) {
        /* Upright: the number, the name, four rows of keys, then Call and
         * Delete. */
        int32_t inner = w - 2 * PHONE_PAD;
        int32_t y = make_display(page, PHONE_PAD, PHONE_PAD / 2, inner, inner) + 2;
        int32_t row_h = (h - PHONE_PAD - y - 4 * PHONE_GAP) / 5;
        if (row_h < 8) row_h = 8;
        int32_t col_w = (inner - 2 * PHONE_GAP) / 3;
        s_kp.grid = make_grid(page, PHONE_PAD, y, inner, 4 * row_h + 3 * PHONE_GAP);
        int32_t bottom = y + 4 * (row_h + PHONE_GAP);
        make_call(page, PHONE_PAD, bottom, inner - col_w - PHONE_GAP, row_h);
        make_delete(page, PHONE_PAD + inner - col_w, bottom, col_w, row_h);
    } else {
        /* Wide: the keys on the right; on the left the number with Delete
         * beside it, the name, and Call. */
        int32_t left_w = (w - 3 * PHONE_PAD) * 2 / 5;
        int32_t right_x = 2 * PHONE_PAD + left_w;
        int32_t btn_h = (h - 2 * PHONE_PAD) / 3;
        if (btn_h > 44) btn_h = 44;
        int32_t btn_y = h - PHONE_PAD - btn_h;
        int32_t del_w = left_w / 3 < DELETE_W ? left_w / 3 : DELETE_W;
        bool name = PHONE_PAD + DISPLAY_H + NAME_H <= btn_y;
        make_display(page, PHONE_PAD, PHONE_PAD, left_w - del_w - PHONE_GAP, name ? left_w : 0);
        make_delete(page, PHONE_PAD + left_w - del_w, PHONE_PAD, del_w, DISPLAY_H);
        s_kp.grid = make_grid(page, right_x, PHONE_PAD, w - right_x - PHONE_PAD, h - 2 * PHONE_PAD);
        make_call(page, PHONE_PAD, btn_y, left_w, btn_h);
    }
    /* "Hold 0 for +" where there is a 0 to hold. */
    s_kp.hint = s_kp.grid != NULL;
    show_number();
    phone_ui_focus(s_kp.display);
}

void phone_keypad_tick(void)
{
    /* The phonebook read since, or changed: the name may be found now. */
    if (s_kp.number && s_number[0] && rs_phonebook_seq() != s_kp.book_seq) show_number();
}

void phone_keypad_release(void)
{
    memset(&s_kp, 0, sizeof s_kp);
}
