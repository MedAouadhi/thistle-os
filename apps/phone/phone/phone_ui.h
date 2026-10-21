/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — what the Phone app's pages share. Internal to the app.
 *
 * phone_ui.c has the frame: the tab bar along the bottom, the page above it,
 * the styles and fonts, and the moves between pages. The pages:
 * phone_lists.c the Favourites, Recents and Contacts tabs, phone_keypad.c
 * the Keypad, phone_contact.c a contact's or a number's page. One page is
 * built at a time, so only its widgets are in LVGL's 64 KB pool; a page left
 * is deleted and built again when it comes back. Everything here runs on
 * the LVGL task.
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PHONE_PAD       6     /* a page's margin */
#define PHONE_GAP       4     /* between buttons */
#define PHONE_BAR_H     30    /* a page's header */
#define PHONE_LIST_MAX  200   /* rows of a list filled at once */
#define PHONE_CELL_PAD  8     /* a list row's side padding */

#define PHONE_STAR        "\xE2\x98\x85"  /* ★ */
#define PHONE_STAR_HOLLOW "\xE2\x98\x86"  /* ☆ */
#define PHONE_ARROW_OUT   "\xE2\x86\x97"  /* ↗ a call made */
#define PHONE_ARROW_IN    "\xE2\x86\x99"  /* ↙ a call received */
#define PHONE_ELLIPSIS    "\xE2\x80\xA6"  /* … */
#define PHONE_DASH        "\xE2\x80\x94"  /* — */

/* The tab bar's pictures, in phone_icons_18 (tools/gen_tab_icons.py). */
#define PHONE_ICON_STAR   "\xEE\x80\x80"  /* U+E000 */
#define PHONE_ICON_CLOCK  "\xEE\x80\x81"  /* U+E001 */
#define PHONE_ICON_PERSON "\xEE\x80\x82"  /* U+E002 */
#define PHONE_ICON_KEYPAD "\xEE\x80\x83"  /* U+E003 */

typedef enum {
    PHONE_TAB_FAVOURITES,
    PHONE_TAB_RECENTS,
    PHONE_TAB_CONTACTS,
    PHONE_TAB_KEYPAD,
    PHONE_TAB_COUNT,
} phone_tab_t;

/* The styles every page draws with, made once each time the app opens and
 * shared by every widget: a style set on one widget at a time costs LVGL's
 * pool for each (CLAUDE.md, "LVGL's pool is 64 KB"). */
typedef struct {
    lv_style_t box;             /* nothing drawn: a container */
    lv_style_t page;            /* filled with the background */
    lv_style_t bar;             /* a header: a rule below */
    lv_style_t button;          /* a 1 px border; its label follows its colour */
    lv_style_t button_pressed;  /* inverted */
    lv_style_t primary;         /* filled: Call */
    lv_style_t primary_pressed; /* not filled */
    lv_style_t focus_ring;      /* where the keyboard is, as an outline */
    lv_style_t text;            /* the Reader's DejaVu, with the app's marks */
    lv_style_t small;           /* Montserrat 14 */
    lv_style_t medium;          /* Montserrat 18 */
    lv_style_t large;           /* Montserrat 22 */
    lv_style_t note;            /* wrapped text */
    lv_style_t field;           /* a text field */
    lv_style_t table;           /* a list */
    lv_style_t cell;            /* its rows */
    lv_style_t cell_chosen;     /* the selected row, inverted */
    lv_style_t scrollbar;
    lv_style_t avatar;          /* a circle */
    lv_style_t grid;            /* the keypad's keys: gaps between */
    lv_style_t rule;            /* a line above */
    lv_style_t flat;            /* a header's button: the header shows through */
    lv_style_t flat_pressed;    /* until pressed */
} phone_styles_t;

const phone_styles_t *phone_ui_styles(void);

/* The font lists and names are set in: the Reader's DejaVu 16 px (its 1-bit
 * cut on e-paper), then the stars and arrows, then Montserrat 14 for LVGL's
 * symbols. */
const lv_font_t *phone_ui_font(void);
/* The tab bar's words and pictures. */
const lv_font_t *phone_ui_tab_font(void);
const lv_font_t *phone_ui_icon_font(void);

bool phone_ui_epaper(void);

/* The width of the app, and the height of a page: above the tab bar. */
int32_t phone_ui_width(void);
int32_t phone_ui_page_height(void);

/* Whether calls can be made here at all (a modem that makes them), and
 * whether there is a Messenger to send an SMS from. */
bool phone_ui_calls_supported(void);
bool phone_ui_messages_supported(void);

/* Widgets with the shared styles. A label takes its parent's colour, so it
 * inverts with a button. */
lv_obj_t *phone_ui_box(lv_obj_t *parent);
lv_obj_t *phone_ui_label(lv_obj_t *parent, const lv_style_t *font, const char *text);
lv_obj_t *phone_ui_button(lv_obj_t *parent, const char *text, const lv_style_t *font, bool primary,
                          lv_event_cb_t clicked, void *user_data);
lv_obj_t *phone_ui_button_label(lv_obj_t *button);
/* Makes a button a header's: no border, no fill of its own until pressed. */
void phone_ui_flat(lv_obj_t *button);
/* A list: one lv_table, `columns` wide, under `page` from `y`, `height` high.
 * Not in the keypad's group: the caller adds it if it takes the keys. */
lv_obj_t *phone_ui_table(lv_obj_t *page, int32_t y, int32_t height, uint32_t columns);

/* Sets a label's text, shaping any Arabic, only when it changed: on e-paper
 * every change is a refresh. */
void phone_ui_set_text(lv_obj_t *label, const char *text);

/* `text`, its Arabic shaped, cut with an ellipsis to fit `width` px in
 * `font`, into `out`. */
void phone_ui_fit(const char *text, const lv_font_t *font, int32_t width, char *out, size_t cap);

/* Puts `obj` in the keypad's group; and focuses it. */
void phone_ui_group_add(lv_obj_t *obj);
void phone_ui_focus(lv_obj_t *obj);

/* Whether the event being handled came from the keyboard. */
bool phone_ui_from_keys(void);

/* Moving between pages. Each deletes the page shown and builds the next. */
void phone_ui_show_tab(phone_tab_t tab);
void phone_ui_open_contact(uint32_t index);
/* A number's page: its contact's, if it is one. */
void phone_ui_open_number(const char *number);
/* Up a level: off a contact's page, or out of a question a page asks. False
 * on a tab's own page, where Back leaves the app. */
bool phone_ui_back(void);
phone_tab_t phone_ui_tab(void);
const char *phone_ui_tab_name(phone_tab_t tab);

/* Place a call, saying why not when it cannot be; logs "call <number>".
 * True when the kernel took it: the call screen comes up on its own. */
bool phone_ui_call(const char *number);
/* Open the Messenger on an SMS to `number`, which is passed on without its
 * spaces and punctuation; logs "message <number>" so. */
void phone_ui_message(const char *number, const char *name);

/* The pages (phone_lists.c, phone_keypad.c, phone_contact.c). build makes
 * the page's widgets under `page`; tick runs every second while it is shown;
 * release forgets the widgets, which are about to be deleted; back answers
 * Back before the frame does. */
void phone_lists_build(lv_obj_t *page, phone_tab_t tab);
void phone_lists_tick(void);
void phone_lists_release(void);
bool phone_lists_back(void);
void phone_lists_top(void);    /* the tab tapped again: to the top */
void phone_lists_forget(void); /* the app closed: no query, no place kept */

void phone_keypad_build(lv_obj_t *page);
void phone_keypad_tick(void);
void phone_keypad_release(void);

bool phone_contact_set_index(uint32_t index);   /* false: no such contact now */
void phone_contact_set_number(const char *number);
void phone_contact_build(lv_obj_t *page);
void phone_contact_tick(void);
void phone_contact_release(void);
