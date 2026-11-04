/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Chat (Tailnet chat) screens
 *
 * Three tabs, each a list, and the pages they open:
 *
 *   ┌──────────────────────────────┐
 *   │ Chats                    [⚙] │  the title; Settings
 *   │ ⚠ Tailscale is off. ...      │  only while chat is unavailable
 *   │ (L) Lena              23:18  │  one lv_table: a monogram, the name,
 *   │     ✓✓ See you then       ②  │  when, the latest and the unread
 *   │ (B) Ben           Yesterday  │
 *   ├──────────────────────────────┤
 *   │ [Chats 2] Contacts  Requests │  the tabs, counting what waits
 *   └──────────────────────────────┘
 *
 *   ┌──────────────────────────────┐
 *   │ <  (L) Lena             [☎]  │  back, the contact (its page), a call
 *   │        Online                │  typing, sending, online...
 *   │          ( Yesterday )       │  a heading for each day
 *   │ ╭──────────────╮             │  theirs outlined on the left
 *   │ │ Are you com… │             │
 *   │ ╰──────────────╯             │
 *   │      ╭─────────────────────╮ │  mine black on the right, with
 *   │      │ Yes! 10:02 ✓✓       │ │  the time and its ticks
 *   │      ╰─────────────────────╯ │
 *   │                       (👍)   │  a reaction hangs from its bubble
 *   │ ( Message            ) [🎤]  │  the microphone is Send once typed
 *   └──────────────────────────────┘
 *
 * - Chats: each contact with messages; a row opens the conversation: its
 *   latest 20 messages, one lv_table of bubbles, each with what it replies
 *   to, its time, its ticks and its reactions. A tap on a bubble opens its
 *   sheet (the reactions, Reply, Play, Read all), a long press or Enter
 *   too, and a swipe to the right replies to it; a tap on a voice message
 *   plays it. While the conversation is on screen its messages are read
 *   (read receipts go if allowed) and alert nothing; typing tells the
 *   contact. Held, the microphone records a voice message, sent when let
 *   go.
 * - Contacts: Look for chat peers on the tailnet, Add by name or address,
 *   then each contact, flagged if another key came under its name or
 *   address. A row opens the contact: Message, Call, its fingerprint,
 *   Rename, Accept new key, Remove.
 * - Requests: each contact request; a row opens it: its fingerprint and
 *   everything it held, as bubbles, then Decline and Accept.
 * - Settings: this device's name and fingerprint, read receipts, and how
 *   often to look for messages while asleep.
 *
 * Made for e-paper first, where every change is a refresh of up to a second
 * and there are only black and white: no animation, nothing grey, and a
 * page is drawn again only when what it shows has changed. Its lists are
 * compared row by row with what is on screen, so a contact that starts
 * typing redraws no list. A conversation's black bubbles leave a ghost on
 * the panel's fast refreshes, so leaving one asks the panel for a full one.
 *
 * Each list is one lv_table, whose rows cost LVGL's 64 KB pool their text
 * alone: the table holds a few characters for each row, enough for its
 * height (a bubble's text, as it wraps), and draw_list_row() and
 * draw_bubble() draw the rest from the page's rows in PSRAM, in the table's
 * LV_EVENT_DRAW_TASK_ADDED.
 *
 * The app reads and asks the kernel (thistle/tailnet_chat.h) and keeps
 * nothing of its own. A timer looks at tailnet_chat_generation() twice a
 * second. Every page change from a click goes through lv_async_call(), as
 * it deletes the widget whose event is running.
 */
#include "tailchat/tailchat_app.h"
#include "tailchat/tailchat_core.h"
#include "tailchat/tailchat_icons.h"
#include "tailchat/tailchat_voice.h"

#include "thistle/tailnet_chat.h"
#include "thistle/time_manager.h"
#include "hal/board.h"
#include "ui/manager.h"
#include "ui/theme.h"
#include "ui/toast.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "tailchat";

LV_FONT_DECLARE(tailchat_icons_14)
LV_FONT_DECLARE(tailchat_icons_14_mono)
LV_FONT_DECLARE(tailchat_icons_20)
LV_FONT_DECLARE(tailchat_icons_20_mono)
LV_FONT_DECLARE(tailchat_emoji_14)
LV_FONT_DECLARE(tailchat_emoji_14_mono)
LV_FONT_DECLARE(tailchat_emoji_20)
LV_FONT_DECLARE(tailchat_emoji_20_mono)

#define ROWS_MAX   34   /* a list's rows: 32 contacts and two actions, or the requests */
#define SHOWN_MAX  20   /* messages in a conversation, as the kernel keeps them */
#define CONV_ROWS  (SHOWN_MAX * 2 + 1)  /* each message, a day heading before it, the unread line */
#define TEXT_MAX   200  /* characters of a message in its bubble, some ten lines; Read all has the rest */
#define TEXT_LINES 12   /* and its lines */
#define TEXT_BYTES 720
#define TYPING_REPEAT_MS 4000  /* "typing" said again while it goes on */
#define TYPING_STOP_MS   5000  /* and "stopped" this long after the last key */

/* Sizes, in pixels. */
#define EDGE        8   /* the page's side margin */
#define AVATAR      36  /* a list row's monogram */
#define ROW_TEXT_X  (10 + AVATAR + 10)
#define ROW_PAD     10  /* above and below a list row's two lines, as the table counts them */
#define BUBBLE_GAP  3   /* above and below a bubble, inside its row */
#define BUBBLE_PX   9   /* a bubble's padding, sideways */
#define BUBBLE_PY   5   /* and up and down */
#define BUBBLE_R    12
#define BUBBLE_AWAY 40  /* the side a bubble leaves free: theirs the right */
#define META_GAP    8   /* between a bubble's text and its time */
#define VOICE_W     150 /* a voice message's bubble, inside */
#define WAVE_BARS   26
#define BUTTON      34  /* a round button */

typedef enum {
    PAGE_CHATS,
    PAGE_CONTACTS,
    PAGE_REQUESTS,
    PAGE_CONVERSATION,
    PAGE_CONTACT,
    PAGE_REQUEST,
    PAGE_ADD,
    PAGE_FIND,
    PAGE_SETTINGS,
    PAGE_MESSAGE,
} page_t;

#define TABS 3
static const char *const TAB_NAMES[TABS] = {"Chats", "Contacts", "Requests"};
static const char *const TAB_ICONS[TABS] = {TC_ICON_CHAT, TC_ICON_PERSON, TC_ICON_PERSON_ADD};

/* A row of a list, as draw_list_row() draws it. */
typedef enum {
    ROW_CHAT,
    ROW_CONTACT,
    ROW_REQUEST,
    ROW_FIND,      /* Contacts: Look for chat peers */
    ROW_ADD,       /* Contacts: Add by name or address */
    ROW_FOUND,
    ROW_RECEIPTS,  /* Settings */
    ROW_CHECK,
} row_kind_t;

typedef enum {
    AVATAR_SOLID,   /* a contact: its key is pinned */
    AVATAR_HOLLOW,  /* a stranger, or a contact never reached */
} avatar_t;

typedef struct {
    uint8_t  kind;     /* row_kind_t */
    uint8_t  avatar;   /* avatar_t */
    uint8_t  tick;     /* tailchat_tick_t of the latest, if this device wrote it */
    bool     online;
    bool     on;       /* a setting's switch */
    uint16_t badge;
    char     badge_text[8];
    char     key[65];
    char     mono[12]; /* the monogram: initials, or a picture */
    char     title[96];
    char     right[24];
    char     detail[200];
} list_row_t;

/* A row of a conversation (or of a request's messages). */
typedef enum {
    BUBBLE_DAY,
    BUBBLE_UNREAD,
    BUBBLE_MESSAGE,
    BUBBLE_EMPTY,
} bubble_kind_t;

typedef struct {
    uint8_t  kind;       /* bubble_kind_t */
    uint8_t  tick;       /* tailchat_tick_t */
    bool     mine;
    bool     tail;       /* the last of its run: a square corner, and its time */
    bool     meta_line;  /* its time on a line of its own, not beside the text */
    bool     cut;        /* longer than shown: Read all */
    int16_t  inner_w;    /* its text's width inside the bubble */
    int16_t  wrap_w;     /* the width its text wraps at */
    int16_t  text_h;
    uint32_t voice_ms;
    char     length[12]; /* a voice message's, "0:07" */
    char     id[33];
    char     meta[24];   /* "14:05" and the ticks */
    char     quote[120]; /* what it replies to, fitted to one line */
    char     react[80];  /* the reactions, "👍 ❤️" */
    char     text[TEXT_BYTES];
} bubble_t;

/* What the pages read from the kernel, in PSRAM while the app is open. */
typedef struct {
    list_row_t rows[ROWS_MAX];
    list_row_t next_rows[ROWS_MAX];
    bubble_t   bubbles[CONV_ROWS];
    bubble_t   next_bubbles[CONV_ROWS];
    uint8_t    waves[WAVE_BARS];
    char       text[4097];   /* one message's text */
    char       quote[4097];  /* the text a reply quotes */
    char       line[900];
    tailnet_chat_contact_t contact;
    tailnet_chat_request_t request;
} store_t;

typedef struct {
    page_t page;
    char   key[65];
} place_t;

static struct {
    lv_obj_t   *root;
    lv_obj_t   *header;
    lv_obj_t   *note;
    lv_obj_t   *body;
    lv_obj_t   *footer;
    lv_obj_t   *table;
    lv_obj_t   *tab_badges[TABS];
    lv_obj_t   *field;       /* the conversation's compose field; the contact page's name;
                                the add page's address */
    lv_obj_t   *field2;      /* the add page's name */
    lv_obj_t   *remove;      /* the contact page's Remove */
    lv_obj_t   *title;       /* the conversation's name */
    lv_obj_t   *presence;    /* typing, online, sending... */
    lv_obj_t   *composer;
    lv_obj_t   *action;      /* the composer's microphone, or Send */
    lv_obj_t   *recording;   /* in the field's place while recording */
    lv_obj_t   *sheet;       /* what can be done with the message chosen */
    lv_obj_t   *reply_bar;   /* what the message being written replies to */
    lv_obj_t   *look_status; /* the find page's progress */
    lv_obj_t   *look_again;
    lv_timer_t *timer;
    store_t    *store;
    page_t      page;
    char        key[65];     /* the page's chat peer */
    place_t     stack[4];    /* the pages back to the tab */
    int         depth;
    place_t     next;
    bool        next_push;
    int         rows;        /* rows of the list or conversation on screen */
    int         messages;    /* messages in the conversation on screen */
    uint32_t    generation;
    char        conv_key[65];/* the conversation on screen's contact */
    char        chosen[33];  /* the message chosen in the conversation */
    char        reply_to[33];/* the message the one being written answers */
    char        read_id[33]; /* the message Read all shows */
    uint16_t    unread_at_open;
    bool        typing;      /* "typing" said to the contact */
    uint32_t    typing_said; /* when, lv_tick */
    uint32_t    typed;       /* the last key in the compose field, lv_tick */
    bool        long_pressed;
    bool        confirm_remove;
    bool        epaper;
    bool        compact;     /* a short screen: smaller titles */
    bool        tiny;        /* a very short one: no tab titles, no names under the pictures */
    int32_t     width;       /* the body's */
    char        rec_key[65]; /* whom the recording under way goes to */
    char        rec_reply[33];/* and what it answers */
    char        playing[33]; /* the voice message playing, as last shown */
    uint32_t    rec_shown_s; /* the recording's seconds, as last shown */
} s_ui;

static struct {
    lv_style_t page;
    lv_style_t header;
    lv_style_t title;
    lv_style_t subtitle;
    lv_style_t note;
    lv_style_t banner;
    lv_style_t body;
    lv_style_t table;
    lv_style_t cell;
    lv_style_t footer;
    lv_style_t tab;
    lv_style_t tab_on;
    lv_style_t tab_pic;
    lv_style_t badge;
    lv_style_t badge_on;
    lv_style_t button;
    lv_style_t button_primary;
    lv_style_t button_pressed;
    lv_style_t icon_button;
    lv_style_t icon_pressed;
    lv_style_t focus_ring;
    lv_style_t field;
    lv_style_t card;
    lv_style_t warn;
    lv_style_t section;
    lv_style_t avatar;
    lv_style_t center;
} s_styles;
static bool s_styles_made;

/* Montserrat, then the app's pictures, then emoji: what LVGL's fonts lack
 * comes from the next (lv_font_t.fallback), so a message's emoji show. */
static lv_font_t s_icons;    /* tailchat_icons_14 */
static lv_font_t s_emoji;    /* tailchat_emoji_14 */
static lv_font_t s_font;     /* Montserrat 14, then s_icons, then s_emoji */
static lv_font_t s_font_18;  /* Montserrat 18, then s_icons... */
static lv_font_t s_font_22;
static lv_font_t s_buttons;  /* tailchat_icons_20, then Montserrat 18 for LV_SYMBOL_* */
static lv_font_t s_reacts;   /* tailchat_emoji_20, then s_buttons for the heart */

/* ------------------------------------------------------------------ */
/* Fonts and styles                                                     */
/* ------------------------------------------------------------------ */

static void fonts_init(void)
{
    bool mono = s_ui.epaper;
    s_emoji = mono ? tailchat_emoji_14_mono : tailchat_emoji_14;
    s_emoji.fallback = NULL;
    s_icons = mono ? tailchat_icons_14_mono : tailchat_icons_14;
    s_icons.fallback = &s_emoji;
    s_font = lv_font_montserrat_14;
    s_font.fallback = &s_icons;
    s_font_18 = lv_font_montserrat_18;
    s_font_18.fallback = &s_icons;
    s_font_22 = lv_font_montserrat_22;
    s_font_22.fallback = &s_icons;
    s_buttons = mono ? tailchat_icons_20_mono : tailchat_icons_20;
    s_buttons.fallback = &lv_font_montserrat_18;
    s_reacts = mono ? tailchat_emoji_20_mono : tailchat_emoji_20;
    s_reacts.fallback = &s_buttons;
}

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
    lv_style_set_pad_row(&s_styles.page, 0);
    lv_style_set_text_color(&s_styles.page, clr->text);
    lv_style_set_text_font(&s_styles.page, &s_font);

    lv_style_set_border_side(&s_styles.header, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&s_styles.header, clr->text);
    lv_style_set_border_width(&s_styles.header, 1);
    lv_style_set_pad_hor(&s_styles.header, 4);
    lv_style_set_pad_ver(&s_styles.header, 3);
    lv_style_set_pad_column(&s_styles.header, 4);

    lv_style_set_text_font(&s_styles.title, s_ui.compact ? &s_font_18 : &s_font_22);
    lv_style_set_text_color(&s_styles.title, clr->text);

    lv_style_set_text_font(&s_styles.subtitle, &s_font);
    lv_style_set_text_color(&s_styles.subtitle, clr->text_secondary);

    lv_style_set_text_color(&s_styles.note, clr->text_secondary);
    lv_style_set_pad_hor(&s_styles.note, EDGE + 4);
    lv_style_set_pad_ver(&s_styles.note, 4);

    /* What is wrong, in a box under the title. */
    lv_style_set_border_color(&s_styles.banner, clr->text);
    lv_style_set_border_width(&s_styles.banner, 1);
    lv_style_set_radius(&s_styles.banner, 8);
    lv_style_set_pad_hor(&s_styles.banner, 8);
    lv_style_set_pad_ver(&s_styles.banner, 5);
    lv_style_set_margin_left(&s_styles.banner, EDGE);
    lv_style_set_margin_right(&s_styles.banner, EDGE);
    lv_style_set_margin_top(&s_styles.banner, 6);
    lv_style_set_text_color(&s_styles.banner, clr->text);

    lv_style_set_pad_all(&s_styles.body, 0);
    lv_style_set_pad_row(&s_styles.body, 6);
    lv_style_set_pad_bottom(&s_styles.body, 8);

    lv_style_set_bg_opa(&s_styles.table, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_styles.table, 0);
    lv_style_set_pad_all(&s_styles.table, 0);

    /* A table's own cell is the page's colour with nothing on it, pressed
     * or not: a tap there flashes nothing on e-paper. */
    lv_style_set_bg_color(&s_styles.cell, clr->bg);
    lv_style_set_bg_opa(&s_styles.cell, LV_OPA_COVER);
    lv_style_set_border_width(&s_styles.cell, 0);
    lv_style_set_text_color(&s_styles.cell, clr->text);
    lv_style_set_text_font(&s_styles.cell, &s_font);

    lv_style_set_border_side(&s_styles.footer, LV_BORDER_SIDE_TOP);
    lv_style_set_border_color(&s_styles.footer, clr->text);
    lv_style_set_border_width(&s_styles.footer, 1);
    lv_style_set_pad_hor(&s_styles.footer, 6);
    lv_style_set_pad_ver(&s_styles.footer, 4);
    lv_style_set_pad_gap(&s_styles.footer, 4);

    lv_style_set_radius(&s_styles.tab, 8);
    lv_style_set_pad_all(&s_styles.tab, 1);
    lv_style_set_pad_row(&s_styles.tab, 1);
    lv_style_set_text_color(&s_styles.tab, clr->text);
    lv_style_set_text_font(&s_styles.tab, &s_font);

    lv_style_set_radius(&s_styles.tab_pic, LV_RADIUS_CIRCLE);
    lv_style_set_width(&s_styles.tab_pic, 46);
    lv_style_set_height(&s_styles.tab_pic, 24);
    lv_style_set_text_font(&s_styles.tab_pic, &s_buttons);

    /* The tab shown is ink with paper letters. */
    lv_style_set_bg_color(&s_styles.tab_on, clr->text);
    lv_style_set_bg_opa(&s_styles.tab_on, LV_OPA_COVER);
    lv_style_set_text_color(&s_styles.tab_on, clr->bg);

    lv_style_set_bg_color(&s_styles.badge, clr->text);
    lv_style_set_bg_opa(&s_styles.badge, LV_OPA_COVER);
    lv_style_set_text_color(&s_styles.badge, clr->bg);
    lv_style_set_radius(&s_styles.badge, LV_RADIUS_CIRCLE);
    lv_style_set_pad_hor(&s_styles.badge, 4);
    lv_style_set_pad_ver(&s_styles.badge, 0);
    lv_style_set_min_width(&s_styles.badge, 16);
    lv_style_set_text_align(&s_styles.badge, LV_TEXT_ALIGN_CENTER);

    /* Over the tab shown, whose picture is ink, a ring of paper parts the
     * two. */
    lv_style_set_border_color(&s_styles.badge_on, clr->bg);
    lv_style_set_border_width(&s_styles.badge_on, 2);

    lv_style_set_bg_color(&s_styles.button, clr->bg);
    lv_style_set_bg_opa(&s_styles.button, LV_OPA_COVER);
    lv_style_set_border_color(&s_styles.button, clr->text);
    lv_style_set_border_width(&s_styles.button, 1);
    lv_style_set_radius(&s_styles.button, LV_RADIUS_CIRCLE);
    lv_style_set_pad_hor(&s_styles.button, 14);
    lv_style_set_pad_ver(&s_styles.button, 7);
    lv_style_set_text_color(&s_styles.button, clr->text);
    lv_style_set_text_font(&s_styles.button, &s_font);

    lv_style_set_bg_color(&s_styles.button_primary, clr->text);
    lv_style_set_text_color(&s_styles.button_primary, clr->bg);

    /* Pressed is a heavier ring, not an inverted button: on e-paper that
     * would flash, and the page usually changes a moment later anyway. */
    lv_style_set_border_width(&s_styles.button_pressed, 3);
    lv_style_set_border_color(&s_styles.button_pressed, clr->text);

    lv_style_set_bg_opa(&s_styles.icon_button, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_styles.icon_button, 0);
    lv_style_set_border_color(&s_styles.icon_button, clr->text);
    lv_style_set_radius(&s_styles.icon_button, LV_RADIUS_CIRCLE);
    lv_style_set_text_color(&s_styles.icon_button, clr->text);
    lv_style_set_text_font(&s_styles.icon_button, &s_buttons);
    lv_style_set_pad_all(&s_styles.icon_button, 0);

    lv_style_set_border_width(&s_styles.icon_pressed, 2);

    lv_style_set_outline_width(&s_styles.focus_ring, 2);
    lv_style_set_outline_color(&s_styles.focus_ring, clr->primary);
    lv_style_set_outline_pad(&s_styles.focus_ring, 1);

    lv_style_set_radius(&s_styles.field, LV_RADIUS_CIRCLE);
    lv_style_set_pad_hor(&s_styles.field, 12);
    lv_style_set_pad_ver(&s_styles.field, 7);
    lv_style_set_text_font(&s_styles.field, &s_font);

    lv_style_set_border_color(&s_styles.card, clr->text);
    lv_style_set_border_width(&s_styles.card, 1);
    lv_style_set_radius(&s_styles.card, 10);
    lv_style_set_pad_all(&s_styles.card, 8);
    lv_style_set_pad_row(&s_styles.card, 2);
    lv_style_set_margin_left(&s_styles.card, EDGE);
    lv_style_set_margin_right(&s_styles.card, EDGE);

    lv_style_set_text_color(&s_styles.warn, clr->text);
    lv_style_set_border_side(&s_styles.warn, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_width(&s_styles.warn, 3);
    lv_style_set_border_color(&s_styles.warn, clr->text);
    lv_style_set_pad_left(&s_styles.warn, 8);
    lv_style_set_margin_left(&s_styles.warn, EDGE);

    lv_style_set_text_color(&s_styles.section, clr->text_secondary);
    lv_style_set_pad_hor(&s_styles.section, EDGE + 4);
    lv_style_set_pad_top(&s_styles.section, 6);

    lv_style_set_radius(&s_styles.avatar, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&s_styles.avatar, clr->text);
    lv_style_set_bg_opa(&s_styles.avatar, LV_OPA_COVER);
    lv_style_set_border_color(&s_styles.avatar, clr->text);
    lv_style_set_border_width(&s_styles.avatar, 2);
    lv_style_set_text_color(&s_styles.avatar, clr->bg);
    lv_style_set_pad_all(&s_styles.avatar, 0);

    lv_style_set_text_align(&s_styles.center, LV_TEXT_ALIGN_CENTER);
}

/* ------------------------------------------------------------------ */
/* Little things                                                        */
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

static void join_group(lv_obj_t *obj)
{
    lv_group_t *group = lv_group_get_default();
    if (group) lv_group_add_obj(group, obj);
}

static void focus(lv_obj_t *obj)
{
    lv_group_t *group = lv_group_get_default();
    if (group && obj) lv_group_focus_obj(obj);
}

static lv_obj_t *plain(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, lv_style_t *style)
{
    lv_obj_t *l = lv_label_create(parent);
    if (style) lv_obj_add_style(l, style, LV_PART_MAIN);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, text);
    return l;
}

/* A label of one line, "..." where it is cut. */
static lv_obj_t *line_label(lv_obj_t *parent, const char *text, lv_style_t *style)
{
    lv_obj_t *l = label(parent, text, style);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

/* A label inside something, in its colour and font: the theme's own
 * label style would keep it ink on an inverted button. */
static lv_obj_t *inner_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_style_all(l);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, bool primary)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &s_styles.button, LV_PART_MAIN);
    if (primary) lv_obj_add_style(b, &s_styles.button_primary, LV_PART_MAIN);
    lv_obj_add_style(b, &s_styles.button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(b, &s_styles.focus_ring, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(inner_label(b, text));
    join_group(b);
    return b;
}

/* A round button with a picture: s_buttons' glyphs or an LV_SYMBOL. */
static lv_obj_t *icon_button(lv_obj_t *parent, const char *icon, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &s_styles.icon_button, LV_PART_MAIN);
    lv_obj_add_style(b, &s_styles.icon_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(b, &s_styles.focus_ring, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_size(b, s_ui.tiny ? 28 : BUTTON, s_ui.tiny ? 28 : BUTTON);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(inner_label(b, icon));
    join_group(b);
    return b;
}

static int32_t text_width(const char *text, const lv_font_t *font)
{
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    return size.x;
}

/* `text` on one line, cut with "..." to `max_w` pixels in `font`. */
static void fit_line(char *out, size_t n, const char *text, const lv_font_t *font, int32_t max_w)
{
    tailchat_one_line(out, n, text, n);
    if (text_width(out, font) <= max_w) return;
    /* The longest start that fits with its "...", found by halves, cut
     * only between characters. */
    size_t len = strlen(out);
    size_t lo = 0, hi = len;
    char *probe = s_ui.store->line;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        while (mid > 0 && ((unsigned char)out[mid] & 0xC0) == 0x80) mid--;
        if (mid <= lo) {
            hi = lo;
            break;
        }
        snprintf(probe, sizeof s_ui.store->line, "%.*s...", (int)mid, out);
        if (text_width(probe, font) <= max_w) lo = mid;
        else hi = mid - 1;
    }
    while (lo > 0 && out[lo - 1] == ' ') lo--;
    /* lo + 3 bytes fit: "..." fitted in out's place before. */
    if (lo + 4 > n) lo = n > 4 ? n - 4 : 0;
    memcpy(out + lo, "...", 4);
}

static bool find_contact(const char *key, tailnet_chat_contact_t *out)
{
    int n = tailnet_chat_contact_count();
    for (int i = 0; i < n; i++) {
        if (tailnet_chat_get_contact(i, out) == ESP_OK && strcmp(out->key, key) == 0) return true;
    }
    return false;
}

static bool find_request(const char *key, tailnet_chat_request_t *out)
{
    int n = tailnet_chat_request_count();
    for (int i = 0; i < n; i++) {
        if (tailnet_chat_get_request(i, out) == ESP_OK && strcmp(out->key, key) == 0) return true;
    }
    return false;
}

/* The clock's now and whether it is set: before 2026 it is not. */
static bool now_tm(struct tm *now, bool *h24)
{
    time_t t = time(NULL);
    localtime_r(&t, now);
    time_status_t ts;
    *h24 = true;
    bool known = now->tm_year + 1900 >= 2026;
    if (time_manager_get_status(&ts) == ESP_OK) {
        *h24 = ts.format_24h;
        known = known && ts.known;
    }
    return known;
}

static void ms_tm(uint64_t ms, struct tm *out)
{
    time_t t = (time_t)(ms / 1000);
    localtime_r(&t, out);
}

static const char *tick_icon(uint8_t tick)
{
    switch (tick) {
    case TAILCHAT_TICK_WAITING:   return TC_ICON_CLOCK;
    case TAILCHAT_TICK_DELIVERED: return TC_ICON_TICK;
    case TAILCHAT_TICK_READ:      return TC_ICON_TICKS;
    default:                      return "";
    }
}

/* ------------------------------------------------------------------ */
/* Going between pages                                                  */
/* ------------------------------------------------------------------ */

static void show_page(void);

static bool is_tab(page_t page)
{
    return page <= PAGE_REQUESTS;
}

static void request_full_refresh(void)
{
    const hal_registry_t *reg = hal_get_registry();
    if (!s_ui.epaper || !reg || !reg->display || !reg->display->set_refresh_mode) return;
    reg->display->set_refresh_mode(HAL_DISPLAY_REFRESH_FULL);
    ESP_LOGI(TAG, "a full refresh, for the conversation's ghost");
}

static void go_async(void *arg)
{
    (void)arg;
    if (!s_ui.root) return;
    if (is_tab(s_ui.next.page)) {
        s_ui.depth = 0;
    } else if (s_ui.next_push) {
        /* Back to a page already beneath, rather than round again. */
        int at = -1;
        for (int i = 0; i < s_ui.depth; i++) {
            if (s_ui.stack[i].page == s_ui.next.page && strcmp(s_ui.stack[i].key, s_ui.next.key) == 0) at = i;
        }
        if (at >= 0) {
            s_ui.depth = at;
        } else if (s_ui.depth < (int)(sizeof s_ui.stack / sizeof s_ui.stack[0])) {
            s_ui.stack[s_ui.depth].page = s_ui.page;
            memcpy(s_ui.stack[s_ui.depth].key, s_ui.key, sizeof s_ui.key);
            s_ui.depth++;
        }
    }
    if (s_ui.page == PAGE_CONVERSATION && s_ui.next.page != PAGE_CONVERSATION) request_full_refresh();
    s_ui.page = s_ui.next.page;
    memcpy(s_ui.key, s_ui.next.key, sizeof s_ui.key);
    s_ui.confirm_remove = false;
    show_page();
}

/* To `page`, about `key`: a tab starts again from it; a page opened from
 * another goes back to it. */
static void go_to(page_t page, const char *key, bool push)
{
    s_ui.next.page = page;
    snprintf(s_ui.next.key, sizeof s_ui.next.key, "%s", key ? key : "");
    s_ui.next_push = push;
    lv_async_call(go_async, NULL);
}

static void go(page_t page, const char *key)
{
    go_to(page, key, true);
}

/* In this page's place: back from the new one goes where this went. */
static void go_instead(page_t page, const char *key)
{
    go_to(page, key, false);
}

static void go_back(void)
{
    if (s_ui.depth > 0) {
        s_ui.depth--;
        go_to(s_ui.stack[s_ui.depth].page, s_ui.stack[s_ui.depth].key, false);
    } else {
        go_to(PAGE_CHATS, NULL, false);
    }
}

static void close_sheet(void);
static void show_reply_bar(void);

static bool back_cb(void)
{
    /* First what is open on the page, then the page. */
    if (s_ui.page == PAGE_CONVERSATION && s_ui.chosen[0]) {
        close_sheet();
        return true;
    }
    if (s_ui.page == PAGE_CONVERSATION && s_ui.reply_to[0]) {
        s_ui.reply_to[0] = '\0';
        show_reply_bar();
        return true;
    }
    if (is_tab(s_ui.page)) return false;
    go_back();
    return true;
}

static void back_button_cb(lv_event_t *e)
{
    (void)e;
    back_cb();
}

static void tab_cb(lv_event_t *e)
{
    page_t page = (page_t)(intptr_t)lv_event_get_user_data(e);
    if (page != s_ui.page) go(page, NULL);
}

static void settings_cb(lv_event_t *e)
{
    (void)e;
    go(PAGE_SETTINGS, NULL);
}

/* ------------------------------------------------------------------ */
/* The header, the banner and the tabs                                  */
/* ------------------------------------------------------------------ */

static lv_obj_t *header_row(void)
{
    lv_obj_t *h = plain(s_ui.header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(h, 4, 0);
    return h;
}

/* A tab's header: its name large, Settings on the right. */
static void tab_header(const char *title)
{
    lv_obj_t *h = header_row();
    lv_obj_set_style_pad_left(h, EDGE, 0);
    lv_obj_t *t = line_label(h, title, &s_styles.title);
    lv_obj_set_flex_grow(t, 1);
    icon_button(h, LV_SYMBOL_SETTINGS, settings_cb);
}

/* A page's header: back, its title and what is under it. */
static lv_obj_t *page_header(const char *title, const char *subtitle)
{
    lv_obj_t *h = header_row();
    icon_button(h, LV_SYMBOL_LEFT, back_button_cb);
    lv_obj_t *col = plain(h, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_width(col, 0);
    s_ui.title = line_label(col, title, &s_styles.title);
    if (s_ui.compact || subtitle) lv_obj_set_style_text_font(s_ui.title, &s_font_18, 0);
    s_ui.presence = subtitle && !s_ui.tiny ? line_label(col, subtitle, &s_styles.subtitle) : NULL;
    return h;
}

static void show_status(void)
{
    tailnet_chat_status_t st;
    if (tailnet_chat_get_status(&st) != ESP_OK) {
        memset(&st, 0, sizeof st);
        snprintf(st.why, sizeof st.why, "Tailnet chat has not started.");
    }
    bool listening = st.state == TAILNET_CHAT_LISTENING;
    lv_obj_t *why = lv_obj_get_child(s_ui.note, 0);
    char text[sizeof st.why + 8];
    snprintf(text, sizeof text, "%s%s", listening ? "" : LV_SYMBOL_WARNING "  ", listening ? "" : st.why);
    if (!listening && strcmp(lv_label_get_text(why), text) != 0) ESP_LOGI(TAG, "unavailable: %s", st.why);
    /* Shown on the lists and the conversation, where it explains a wait. */
    bool here = is_tab(s_ui.page) || s_ui.page == PAGE_CONVERSATION || s_ui.page == PAGE_FIND;
    set_text(why, text);
    set_shown(s_ui.note, !listening && here);

    char count[12];
    for (int t = 0; t < TABS; t++) {
        if (!s_ui.tab_badges[t]) continue;
        uint32_t n = t == 0 ? st.unread : t == 2 ? st.requests : 0;
        snprintf(count, sizeof count, n > 99 ? "99+" : "%u", (unsigned)n);
        set_text(s_ui.tab_badges[t], count);
        set_shown(s_ui.tab_badges[t], n > 0);
    }
}

/* The tabs, as a phone's are: a picture over each name, the tab shown
 * with its picture on ink, and the count at the picture's corner. */
static void tab_bar(void)
{
    lv_obj_t *bar = plain(s_ui.footer, LV_FLEX_FLOW_ROW);
    lv_obj_t *pics[TABS];
    for (int t = 0; t < TABS; t++) {
        bool on = (page_t)t == s_ui.page;
        lv_obj_t *b = lv_button_create(bar);
        lv_obj_remove_style_all(b);
        lv_obj_add_style(b, &s_styles.tab, LV_PART_MAIN);
        lv_obj_add_style(b, &s_styles.focus_ring, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, LV_SIZE_CONTENT);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)t);
        lv_obj_t *pic = lv_obj_create(b);
        pics[t] = pic;
        lv_obj_remove_style_all(pic);
        lv_obj_add_style(pic, &s_styles.tab_pic, LV_PART_MAIN);
        if (on) lv_obj_add_style(pic, &s_styles.tab_on, LV_PART_MAIN);
        lv_obj_remove_flag(pic, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_center(inner_label(pic, TAB_ICONS[t]));
        if (!s_ui.tiny) inner_label(b, TAB_NAMES[t]);
        join_group(b);
        /* Chats counts what is unread, Requests what waits. */
        if (t == 1) continue;
        lv_obj_t *badge = inner_label(b, "");
        lv_obj_add_style(badge, &s_styles.badge, LV_PART_MAIN);
        if (on) lv_obj_add_style(badge, &s_styles.badge_on, LV_PART_MAIN);
        lv_obj_add_flag(badge, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
        s_ui.tab_badges[t] = badge;
    }
    /* With no title over the tab, Settings comes here. */
    if (s_ui.tiny) icon_button(bar, LV_SYMBOL_SETTINGS, settings_cb);
    /* Each count over its picture's top right corner, inside its tab. */
    lv_obj_update_layout(bar);
    for (int t = 0; t < TABS; t++) {
        if (s_ui.tab_badges[t]) lv_obj_align_to(s_ui.tab_badges[t], pics[t], LV_ALIGN_TOP_RIGHT, 9, -2);
    }
}

/* ------------------------------------------------------------------ */
/* Drawing in a table's cells                                           */
/* ------------------------------------------------------------------ */

static void draw_text(lv_layer_t *layer, const char *text, const lv_font_t *font, lv_color_t color,
                      lv_text_align_t align, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    if (!text || !text[0]) return;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = text;
    d.font = font;
    d.color = color;
    d.align = align;
    lv_area_t a = {x1, y1, x2, y2};
    lv_draw_label(layer, &d, &a);
}

static void draw_box(lv_layer_t *layer, const lv_area_t *a, int32_t radius, bool filled, lv_color_t fill,
                     int32_t border, lv_color_t border_color)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = radius;
    d.bg_color = fill;
    d.bg_opa = filled ? LV_OPA_COVER : LV_OPA_TRANSP;
    d.border_width = border;
    d.border_color = border_color;
    d.border_opa = border ? LV_OPA_COVER : LV_OPA_TRANSP;
    lv_draw_rect(layer, &d, a);
}

static void draw_hline(lv_layer_t *layer, int32_t x1, int32_t x2, int32_t y, lv_color_t color)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.width = 1;
    d.p1.x = x1;
    d.p1.y = y;
    d.p2.x = x2;
    d.p2.y = y;
    lv_draw_line(layer, &d);
}

/* A monogram: solid for a contact, hollow for a stranger, and a dot if it
 * is connected now. */
static void draw_avatar(lv_layer_t *layer, int32_t x, int32_t y, int32_t size, const char *mono, avatar_t kind,
                        bool online, lv_color_t ink, lv_color_t paper)
{
    lv_area_t a = {x, y, x + size - 1, y + size - 1};
    bool solid = kind == AVATAR_SOLID;
    draw_box(layer, &a, LV_RADIUS_CIRCLE, solid, ink, solid ? 0 : 2, ink);
    /* A picture is the 14 px font's; initials are 18 px. */
    const lv_font_t *font = (unsigned char)mono[0] == 0xEE ? &s_font : &s_font_18;
    int32_t lh = lv_font_get_line_height(font);
    draw_text(layer, mono, font, solid ? paper : ink, LV_TEXT_ALIGN_CENTER, x, y + (size - lh) / 2,
              x + size - 1, y + (size + lh) / 2);
    if (!online) return;
    lv_area_t dot = {x + size - 12, y + size - 12, x + size - 1, y + size - 1};
    draw_box(layer, &dot, LV_RADIUS_CIRCLE, true, ink, 2, paper);
}

/* A pill of text, as a badge or a day heading is. */
static void draw_pill(lv_layer_t *layer, const char *text, int32_t x_center_or_right, int32_t y, bool right_aligned,
                      bool filled, lv_color_t ink, lv_color_t paper)
{
    int32_t lh = lv_font_get_line_height(&s_font);
    int32_t w = text_width(text, &s_font) + 12;
    if (w < lh + 2) w = lh + 2;
    int32_t x1 = right_aligned ? x_center_or_right - w + 1 : x_center_or_right - w / 2;
    lv_area_t a = {x1, y, x1 + w - 1, y + lh + 1};
    draw_box(layer, &a, LV_RADIUS_CIRCLE, true, filled ? ink : paper, filled ? 0 : 1, ink);
    draw_text(layer, text, &s_font, filled ? paper : ink, LV_TEXT_ALIGN_CENTER, a.x1, y + 1, a.x2, a.y2);
}

/* A cell's own text, which only sets its height, is not drawn. Only made
 * clear: Arabic text there is a copy the task frees when it is done
 * (arabic_text.c), so its pointer stays as it is. */
static bool hide_cell_text(lv_draw_task_t *task)
{
    if (lv_draw_task_get_type(task) != LV_DRAW_TASK_TYPE_LABEL) return false;
    lv_draw_task_get_label_dsc(task)->opa = LV_OPA_TRANSP;
    return true;
}

/* The row the keys have chosen, or -1. */
static int key_row(void)
{
    if (!s_ui.table || !lv_obj_has_state(s_ui.table, LV_STATE_FOCUS_KEY)) return -1;
    uint32_t row = LV_TABLE_CELL_NONE, col = LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(s_ui.table, &row, &col);
    return row < (uint32_t)s_ui.rows ? (int)row : -1;
}

/* ------------------------------------------------------------------ */
/* The lists                                                            */
/* ------------------------------------------------------------------ */

static void draw_list_row(lv_event_t *e)
{
    lv_draw_task_t *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(task);
    if (base->part != LV_PART_ITEMS || base->id1 >= (uint32_t)s_ui.rows) return;
    if (hide_cell_text(task) || lv_draw_task_get_type(task) != LV_DRAW_TASK_TYPE_FILL) return;

    const theme_colors_t *clr = theme_get_colors();
    const list_row_t *r = &s_ui.store->rows[base->id1];
    lv_area_t a;
    lv_draw_task_get_area(task, &a);
    lv_layer_t *layer = base->layer;
    bool chosen = key_row() == (int)base->id1;
    lv_color_t ink = chosen ? clr->bg : clr->text;
    lv_color_t paper = chosen ? clr->text : clr->bg;
    if (chosen) {
        lv_area_t hl = {a.x1 + 3, a.y1 + 1, a.x2 - 3, a.y2 - 1};
        draw_box(layer, &hl, 10, true, clr->text, 0, clr->text);
    }

    int32_t lh = lv_font_get_line_height(&s_font);
    int32_t h = lv_area_get_height(&a);
    draw_avatar(layer, a.x1 + 10, a.y1 + (h - AVATAR) / 2, AVATAR, r->mono, (avatar_t)r->avatar, r->online, ink,
                paper);

    /* The name in 18 px over the detail in 14: taller than the two lines
     * of 14 px the table counts, so centred in its padding too. */
    int32_t x = a.x1 + ROW_TEXT_X, right = a.x2 - 10;
    int32_t lh18 = lv_font_get_line_height(&s_font_18);
    int32_t top = a.y1 + (h - lh18 - lh) / 2;
    draw_text(layer, r->title, &s_font_18, ink, LV_TEXT_ALIGN_LEFT, x, top, right, top + lh18);

    int32_t line2 = top + lh18;
    if (r->kind == ROW_RECEIPTS) {
        /* A switch: ink and its knob right when on, a ring and its knob
         * left when off. */
        lv_area_t track = {right - 31, a.y1 + h / 2 - 9, right, a.y1 + h / 2 + 8};
        draw_box(layer, &track, LV_RADIUS_CIRCLE, r->on, ink, 2, ink);
        int32_t kx = r->on ? track.x2 - 14 : track.x1 + 3;
        lv_area_t knob = {kx, track.y1 + 3, kx + 11, track.y2 - 3};
        draw_box(layer, &knob, LV_RADIUS_CIRCLE, true, r->on ? paper : ink, 0, ink);
    } else if (r->kind == ROW_FOUND) {
        draw_pill(layer, r->right, right, a.y1 + h / 2 - lh / 2 - 1, true, true, ink, paper);
    } else if (r->right[0]) {
        draw_text(layer, r->right, &s_font, ink, LV_TEXT_ALIGN_RIGHT, right - 90, top + 2, right, top + 2 + lh);
    }

    int32_t dx = x;
    if (r->tick) {
        draw_text(layer, tick_icon(r->tick), &s_font, ink, LV_TEXT_ALIGN_LEFT, dx, line2, dx + 20, line2 + lh);
        dx += 19;
    }
    draw_text(layer, r->detail, &s_font, ink, LV_TEXT_ALIGN_LEFT, dx, line2, right, line2 + lh);
    if (r->badge) draw_pill(layer, r->badge_text, right, line2, true, true, ink, paper);
    if (!chosen && key_row() != (int)base->id1 + 1) draw_hline(layer, x, a.x2, a.y2, clr->text_secondary);
}

static void choose_row(int row);

/* A row chosen: by a tap or Enter, not the selection moving. */
static void list_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD && lv_indev_get_key(indev) != LV_KEY_ENTER) {
        lv_obj_invalidate(s_ui.table);
        return;
    }
    uint32_t row = LV_TABLE_CELL_NONE, col = LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(s_ui.table, &row, &col);
    if (row < (uint32_t)s_ui.rows) choose_row((int)row);
}

static lv_obj_t *make_table(lv_event_cb_t draw, lv_event_cb_t chosen, int32_t pad_left, int32_t pad_right,
                            int32_t pad_ver)
{
    lv_obj_t *t = lv_table_create(s_ui.body);
    lv_obj_remove_style_all(t);
    lv_obj_add_style(t, &s_styles.table, LV_PART_MAIN);
    lv_obj_add_style(t, &s_styles.cell, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(t, pad_left, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(t, pad_right, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(t, pad_ver, LV_PART_ITEMS);
    lv_obj_set_width(t, LV_PCT(100));
    lv_table_set_column_count(t, 1);
    lv_table_set_column_width(t, 0, s_ui.width);
    lv_table_set_row_count(t, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(t, draw, LV_EVENT_DRAW_TASK_ADDED, NULL);
    if (chosen) {
        lv_obj_add_event_cb(t, chosen, LV_EVENT_VALUE_CHANGED, NULL);
        join_group(t);
    } else {
        lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    }
    return t;
}

static lv_obj_t *make_list(void)
{
    return make_table(draw_list_row, list_cb, ROW_TEXT_X, 10, ROW_PAD);
}

/* The width a row's title and detail have, beside what is on their right. */
static int32_t row_text_width(void)
{
    return s_ui.width - ROW_TEXT_X - 10;
}

static void fill_row(list_row_t *r, row_kind_t kind, const char *key, const char *name, const char *right,
                     const char *detail)
{
    memset(r, 0, sizeof *r);
    r->kind = (uint8_t)kind;
    snprintf(r->key, sizeof r->key, "%s", key ? key : "");
    tailchat_initials(name, r->mono, sizeof r->mono);
    snprintf(r->right, sizeof r->right, "%s", right ? right : "");
    int32_t right_w = r->right[0] ? text_width(r->right, &s_font) + 8 : 0;
    fit_line(r->title, sizeof r->title, name, &s_font_18, row_text_width() - right_w);
    fit_line(r->detail, sizeof r->detail, detail ? detail : "", &s_font, row_text_width());
}

/* A detail line with room for a badge and a tick before it. */
static void refit_detail(list_row_t *r, const char *detail)
{
    snprintf(r->badge_text, sizeof r->badge_text, r->badge > 99 ? "99+" : "%u", (unsigned)r->badge);
    int32_t w = row_text_width() - (r->tick ? 19 : 0);
    if (r->badge) w -= text_width("99", &s_font) + 18;
    fit_line(r->detail, sizeof r->detail, detail, &s_font, w);
}

static int build_chats(list_row_t *rows)
{
    struct tm now;
    bool h24;
    bool known = now_tm(&now, &h24);
    int n = tailnet_chat_contact_count(), count = 0;
    tailnet_chat_contact_t *c = &s_ui.store->contact;
    for (int i = 0; i < n && count < ROWS_MAX; i++) {
        if (tailnet_chat_get_contact(i, c) != ESP_OK) continue;
        int m = tailnet_chat_message_count(c->key);
        if (m <= 0) continue;
        tailnet_chat_message_t msg;
        if (tailnet_chat_get_message(c->key, m - 1, &msg, s_ui.store->text, sizeof s_ui.store->text) != ESP_OK) continue;
        char when[24] = "", detail[160];
        if (known) {
            struct tm then;
            ms_tm(msg.ts, &then);
            tailchat_when(&now, &then, h24, when, sizeof when);
        }
        list_row_t *r = &rows[count++];
        if (c->typing) {
            snprintf(detail, sizeof detail, "typing...");
        } else if (msg.voice_ms) {
            char length[12];
            snprintf(detail, sizeof detail, TC_ICON_MIC " Voice message %s",
                     tailchat_duration(length, sizeof length, msg.voice_ms));
        } else {
            tailchat_one_line(detail, sizeof detail, s_ui.store->text, 80);
        }
        fill_row(r, ROW_CHAT, c->key, c->name, when, "");
        r->avatar = c->pending ? AVATAR_HOLLOW : AVATAR_SOLID;
        r->online = c->connected;
        r->tick = c->typing ? TAILCHAT_TICK_NONE : (uint8_t)tailchat_tick(msg.mine, msg.state);
        r->badge = c->unread;
        refit_detail(r, detail);
    }
    return count;
}

static int build_contacts(list_row_t *rows)
{
    int count = 0;
    fill_row(&rows[count++], ROW_FIND, NULL, "Look for chat peers", NULL, "On your tailnet");
    snprintf(rows[0].mono, sizeof rows[0].mono, TC_ICON_RADAR);
    rows[0].avatar = AVATAR_HOLLOW;
    fill_row(&rows[count++], ROW_ADD, NULL, "Add contact", NULL, "By name or address");
    snprintf(rows[1].mono, sizeof rows[1].mono, LV_SYMBOL_PLUS);
    rows[1].avatar = AVATAR_HOLLOW;

    int n = tailnet_chat_contact_count();
    tailnet_chat_contact_t *c = &s_ui.store->contact;
    for (int i = 0; i < n && count < ROWS_MAX; i++) {
        if (tailnet_chat_get_contact(i, c) != ESP_OK) continue;
        char detail[96];
        if (c->flagged) {
            snprintf(detail, sizeof detail, LV_SYMBOL_WARNING " Another key came under this name");
        } else {
            tailchat_presence_t p = {.pending = c->pending, .connected = c->connected, .seen_name = c->seen_name};
            snprintf(detail, sizeof detail, "%s", tailchat_presence(&p));
            if (!detail[0]) snprintf(detail, sizeof detail, "%s", c->fingerprint);
        }
        list_row_t *r = &rows[count++];
        fill_row(r, ROW_CONTACT, c->key, c->name, NULL, detail);
        r->avatar = c->pending ? AVATAR_HOLLOW : AVATAR_SOLID;
        r->online = c->connected;
    }
    return count;
}

static int build_requests(list_row_t *rows)
{
    int n = tailnet_chat_request_count(), count = 0;
    tailnet_chat_request_t *q = &s_ui.store->request;
    for (int i = 0; i < n && count < ROWS_MAX; i++) {
        if (tailnet_chat_get_request(i, q) != ESP_OK) continue;
        list_row_t *r = &rows[count++];
        fill_row(r, ROW_REQUEST, q->key, q->name, NULL, "");
        r->avatar = AVATAR_HOLLOW;
        r->badge = q->held;
        refit_detail(r, q->preview);
    }
    return count;
}

static int build_found(list_row_t *rows)
{
    int n = tailnet_chat_found_count(), count = 0;
    tailnet_chat_found_t f;
    for (int i = 0; i < n && count < ROWS_MAX; i++) {
        if (tailnet_chat_get_found(i, &f) != ESP_OK) continue;
        char detail[128];
        snprintf(detail, sizeof detail, "%s  %s", f.tailnet_name, f.fingerprint);
        fill_row(&rows[count], ROW_FOUND, f.key, f.name, "Add", detail);
        rows[count++].avatar = AVATAR_HOLLOW;
    }
    return count;
}

static int build_settings(list_row_t *rows)
{
    bool on = tailnet_chat_read_receipts();
    /* Room on the right for the switch. */
    fill_row(&rows[0], ROW_RECEIPTS, NULL, "Read receipts", "       ", "");
    fit_line(rows[0].detail, sizeof rows[0].detail, on ? "Sent as you read" : "Not sent", &s_font,
             row_text_width() - 44);
    snprintf(rows[0].mono, sizeof rows[0].mono, TC_ICON_TICKS);
    rows[0].avatar = AVATAR_HOLLOW;
    rows[0].on = on;
    char value[32];
    uint32_t minutes = tailnet_chat_check_minutes();
    tailchat_check_label(minutes, value, sizeof value);
    value[0] = (char)(value[0] - 'A' + 'a');   /* "Every ..." after "Checks" */
    char detail[48];
    if (minutes) snprintf(detail, sizeof detail, "Checks %s", value);
    else snprintf(detail, sizeof detail, "No checks for messages");
    fill_row(&rows[1], ROW_CHECK, NULL, "While asleep", NULL, detail);
    snprintf(rows[1].mono, sizeof rows[1].mono, TC_ICON_CLOCK);
    rows[1].avatar = AVATAR_HOLLOW;
    return 2;
}

static int build_rows(list_row_t *rows)
{
    switch (s_ui.page) {
    case PAGE_CHATS:    return build_chats(rows);
    case PAGE_CONTACTS: return build_contacts(rows);
    case PAGE_REQUESTS: return build_requests(rows);
    case PAGE_FIND:     return build_found(rows);
    case PAGE_SETTINGS: return build_settings(rows);
    default:            return 0;
    }
}

static const char *page_name(page_t p)
{
    static const char *names[] = {"chats", "contacts", "requests", "conversation", "contact", "request", "add",
                                  "find", "settings", "message"};
    return names[p];
}

static void show_empty(void);

/* The list again, drawn only if a row changed: each draw is a refresh. */
static void refresh_list(bool first)
{
    store_t *st = s_ui.store;
    memset(st->next_rows, 0, sizeof st->next_rows);
    int n = build_rows(st->next_rows);
    if (!first && n == s_ui.rows && memcmp(st->rows, st->next_rows, sizeof st->rows[0] * (size_t)n) == 0) return;
    memcpy(st->rows, st->next_rows, sizeof st->rows);
    s_ui.rows = n;
    lv_table_set_row_count(s_ui.table, (uint32_t)n);
    for (int i = 0; i < n; i++) lv_table_set_cell_value(s_ui.table, (uint32_t)i, 0, "-\n-");
    set_shown(s_ui.table, n > 0);
    lv_obj_invalidate(s_ui.table);
    if (is_tab(s_ui.page)) ESP_LOGI(TAG, "page %s: %d rows", page_name(s_ui.page), s_ui.page == PAGE_CONTACTS ? n - 2 : n);
    tailnet_chat_found_t f;
    for (int i = 0; s_ui.page == PAGE_FIND && i < n; i++) {
        if (tailnet_chat_get_found(i, &f) == ESP_OK) ESP_LOGI(TAG, "found %s (%s) %s", f.name, f.tailnet_name, f.fingerprint);
    }
    show_empty();
}

/* ------------------------------------------------------------------ */
/* The tabs' pages                                                      */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_empty;

static void look_cb(lv_event_t *e);

static void find_page_cb(lv_event_t *e)
{
    look_cb(e);
    go(PAGE_FIND, NULL);
}

/* What a list says with nothing in it, and what to do about it. */
static void show_empty(void)
{
    bool none = s_ui.rows == 0 && (s_ui.page == PAGE_CHATS || s_ui.page == PAGE_REQUESTS);
    if (!none) {
        if (s_empty) {
            lv_obj_delete(s_empty);
            s_empty = NULL;
        }
        return;
    }
    if (s_empty) return;
    s_empty = plain(s_ui.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_empty, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(s_empty, s_ui.compact ? 8 : 28, 0);
    lv_obj_set_style_pad_row(s_empty, 6, 0);
    lv_obj_set_style_pad_hor(s_empty, 16, 0);
    lv_obj_t *pic = lv_label_create(s_empty);
    lv_obj_set_style_text_font(pic, &s_buttons, 0);
    lv_label_set_text(pic, s_ui.page == PAGE_CHATS ? TC_ICON_CHAT : TC_ICON_PERSON);
    bool chats = s_ui.page == PAGE_CHATS;
    lv_obj_t *t = label(s_empty, chats ? "No conversations yet" : "No contact requests", &s_styles.center);
    lv_obj_set_style_text_font(t, &s_font_18, 0);
    label(s_empty,
          chats ? "Look for chat peers on your tailnet, or write to a contact. A stranger's first "
                  "message comes as a request."
                : "A chat peer who is not a contact writes here first.",
          &s_styles.center);
    if (chats) {
        lv_obj_t *b = button(s_empty, TC_ICON_RADAR "  Look for chat peers", find_page_cb, true);
        lv_obj_set_style_margin_top(b, 6, 0);
    }
}

static void tab_page(const char *title)
{
    if (s_ui.tiny) set_shown(s_ui.header, false);
    else tab_header(title);
    s_ui.table = make_list();
    tab_bar();
    refresh_list(true);
    if (s_ui.rows > 0) focus(s_ui.table);
}

/* ------------------------------------------------------------------ */
/* Looking for chat peers on the tailnet                               */
/* ------------------------------------------------------------------ */

static void look_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = tailnet_chat_discover();
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "could not look for chat peers: error 0x%x", (unsigned)err);
        toast_warn("Chat is not on the tailnet yet");
    }
}

static void show_page_async(void *arg)
{
    (void)arg;
    if (s_ui.root) show_page();
}

static void look_again_cb(lv_event_t *e)
{
    look_cb(e);
    lv_async_call(show_page_async, NULL);
}

static void add_found(const char *key)
{
    char added[65];
    esp_err_t err = tailnet_chat_add_found(key, added);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "could not add the chat peer found: error 0x%x", (unsigned)err);
        toast_warn(err == ESP_ERR_NO_MEM ? "Too many contacts" : "Could not add the contact");
        return;
    }
    tailnet_chat_contact_t *c = &s_ui.store->contact;
    if (find_contact(added, c)) ESP_LOGI(TAG, "added %s, found on the tailnet", c->name);
    toast_info("Added; its key is pinned when it is first reached");
    go_instead(PAGE_CONTACT, added);
}

/* How the look goes, above what it found. */
static void show_look(void)
{
    tailnet_chat_discovery_t d = {0};
    tailnet_chat_get_discovery(&d);
    int n = tailnet_chat_found_count();
    char *text = s_ui.store->line;
    if (d.looking) {
        snprintf(text, sizeof s_ui.store->line, TC_ICON_RADAR "  Looking: %u of %u tailnet peers asked...",
                 (unsigned)d.probed, (unsigned)d.peers);
    } else if (!d.looked) {
        snprintf(text, sizeof s_ui.store->line, "Not looked yet.");
    } else if (n == 0) {
        snprintf(text, sizeof s_ui.store->line,
                 "No new chat peer answered among %u tailnet peers online.\n\nOne whose tailnet "
                 "policy keeps port 4810 closed does not answer.", (unsigned)d.peers);
    } else {
        snprintf(text, sizeof s_ui.store->line, "Tap one to add it as a contact.");
    }
    set_text(s_ui.look_status, text);
    if (s_ui.look_again) set_shown(s_ui.look_again, !d.looking);
    ESP_LOGI(TAG, "page find: %s, %u of %u asked, %d rows", d.looking ? "looking" : "done", (unsigned)d.probed,
             (unsigned)d.peers, n);
}

static void show_find(void)
{
    page_header("Chat peers", "On your tailnet");
    s_ui.look_status = label(s_ui.body, "", &s_styles.note);
    s_ui.table = make_list();
    lv_obj_t *row = plain(s_ui.body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_ui.look_again = button(row, TC_ICON_RADAR "  Look again", look_again_cb, false);
    show_look();
    refresh_list(true);
    if (s_ui.rows > 0) focus(s_ui.table);
}

/* ------------------------------------------------------------------ */
/* Settings                                                             */
/* ------------------------------------------------------------------ */

static void own_card(void)
{
    tailnet_chat_status_t st;
    if (tailnet_chat_get_status(&st) != ESP_OK) memset(&st, 0, sizeof st);
    lv_obj_t *card = plain(s_ui.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(card, &s_styles.card, LV_PART_MAIN);
    lv_obj_set_width(card, s_ui.width - 2 * EDGE);
    lv_obj_set_style_margin_top(card, 8, 0);
    line_label(card, st.name[0] ? st.name : "This device", &s_styles.title);
    lv_obj_set_style_text_font(lv_obj_get_child(card, 0), &s_font_18, 0);
    snprintf(s_ui.store->line, sizeof s_ui.store->line, "%s\n" TC_ICON_KEY " %s",
             st.state == TAILNET_CHAT_LISTENING ? "Others add it by this name" : "Not on the tailnet now",
             st.fingerprint[0] ? st.fingerprint : "No identity yet");
    label(card, s_ui.store->line, NULL);
}

static void show_settings(void)
{
    page_header("Settings", NULL);
    own_card();
    s_ui.table = make_list();
    refresh_list(true);
    focus(s_ui.table);
}

/* ------------------------------------------------------------------ */
/* A list's row chosen                                                  */
/* ------------------------------------------------------------------ */

static void choose_row(int row)
{
    const list_row_t *r = &s_ui.store->rows[row];
    switch ((row_kind_t)r->kind) {
    case ROW_CHAT:    go(PAGE_CONVERSATION, r->key); break;
    case ROW_CONTACT: go(PAGE_CONTACT, r->key); break;
    case ROW_REQUEST: go(PAGE_REQUEST, r->key); break;
    case ROW_FIND:    find_page_cb(NULL); break;
    case ROW_ADD:     go(PAGE_ADD, NULL); break;
    case ROW_FOUND:   add_found(r->key); break;
    case ROW_RECEIPTS: {
        bool on = !tailnet_chat_read_receipts();
        tailnet_chat_set_read_receipts(on);
        ESP_LOGI(TAG, "read receipts %s", on ? "on" : "off");
        refresh_list(false);
        break;
    }
    case ROW_CHECK: {
        uint32_t minutes = tailchat_next_check_minutes(tailnet_chat_check_minutes());
        tailnet_chat_set_check_minutes(minutes);
        ESP_LOGI(TAG, "check every %u minutes", (unsigned)minutes);
        refresh_list(false);
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Adding a contact                                                     */
/* ------------------------------------------------------------------ */

static lv_obj_t *field_with_label(const char *caption, const char *placeholder, uint32_t max, lv_event_cb_t ready)
{
    label(s_ui.body, caption, &s_styles.section);
    lv_obj_t *f = lv_textarea_create(s_ui.body);
    lv_obj_add_style(f, &s_styles.field, LV_PART_MAIN);
    lv_textarea_set_one_line(f, true);
    lv_textarea_set_max_length(f, max);
    lv_textarea_set_placeholder_text(f, placeholder);
    lv_obj_set_width(f, s_ui.width - 2 * EDGE);
    lv_obj_set_style_margin_left(f, EDGE, 0);
    if (ready) lv_obj_add_event_cb(f, ready, LV_EVENT_READY, NULL);
    join_group(f);
    return f;
}

static void add_cb(lv_event_t *e)
{
    (void)e;
    char host[64];
    uint16_t port = 0;
    if (!tailchat_parse_address(lv_textarea_get_text(s_ui.field), host, sizeof host, &port)) {
        toast_warn("Type a tailnet name or address, and :port if not 4810");
        return;
    }
    char key[65];
    esp_err_t err = tailnet_chat_add_contact(host, port, lv_textarea_get_text(s_ui.field2), key);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "could not add %s: error 0x%x", host, (unsigned)err);
        toast_warn(err == ESP_ERR_NO_MEM ? "Too many contacts" : "Could not add the contact");
        return;
    }
    ESP_LOGI(TAG, "added %s port %u", host, (unsigned)port);
    toast_info("Added; its key is pinned when it is first reached");
    go_instead(PAGE_CONTACT, key);
}

static void show_add(void)
{
    page_header("Add contact", NULL);
    label(s_ui.body, "Its key is pinned when it is first reached, so the first answer decides who "
                     "this contact is.", &s_styles.note);
    s_ui.field = field_with_label("Tailnet name or address", "homelab or 100.64.0.2", 63 + 6, add_cb);
    s_ui.field2 = field_with_label("Name (optional)", "What to call it", 64, add_cb);
    lv_obj_t *row = plain(s_ui.body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_hor(row, EDGE, 0);
    lv_obj_set_style_pad_top(row, 4, 0);
    lv_obj_t *add = button(row, TC_ICON_PERSON_ADD "  Add contact", add_cb, true);
    lv_obj_set_flex_grow(add, 1);
    focus(s_ui.field);
}

/* ------------------------------------------------------------------ */
/* A conversation                                                       */
/* ------------------------------------------------------------------ */

/* The inside width a bubble may have, and the cells' sides that make its
 * text wrap there. */
static int32_t bubble_max(void)
{
    return s_ui.width - 2 * EDGE - BUBBLE_AWAY - 2 * BUBBLE_PX;
}

static int32_t bubble_side(void)
{
    return (s_ui.width - bubble_max()) / 2;
}

static void choose_message(const char *id, bool sheet);

static void draw_bubble(lv_event_t *e)
{
    lv_draw_task_t *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(task);
    if (base->part != LV_PART_ITEMS || base->id1 >= (uint32_t)s_ui.rows) return;
    if (hide_cell_text(task) || lv_draw_task_get_type(task) != LV_DRAW_TASK_TYPE_FILL) return;

    const theme_colors_t *clr = theme_get_colors();
    const bubble_t *b = &s_ui.store->bubbles[base->id1];
    lv_area_t a;
    lv_draw_task_get_area(task, &a);
    lv_layer_t *layer = base->layer;
    int32_t lh = lv_font_get_line_height(&s_font);
    int32_t mid = (a.x1 + a.x2) / 2;

    if (b->kind == BUBBLE_DAY || b->kind == BUBBLE_EMPTY) {
        draw_pill(layer, b->text, mid, a.y1 + BUBBLE_GAP + BUBBLE_PY - 1, false, false, clr->text, clr->bg);
        return;
    }
    if (b->kind == BUBBLE_UNREAD) {
        int32_t w = text_width(b->text, &s_font) / 2 + 8;
        int32_t y = a.y1 + BUBBLE_GAP + BUBBLE_PY + lh / 2;
        draw_hline(layer, a.x1 + EDGE, mid - w, y, clr->text);
        draw_hline(layer, mid + w, a.x2 - EDGE, y, clr->text);
        draw_text(layer, b->text, &s_font, clr->text, LV_TEXT_ALIGN_CENTER, mid - w, y - lh / 2, mid + w, y + lh / 2);
        return;
    }

    int32_t inner_h = (b->quote[0] ? lh : 0) + b->text_h + (b->meta_line ? lh : 0);
    if (b->voice_ms) inner_h = 2 * lh;
    int32_t w = b->inner_w + 2 * BUBBLE_PX;
    lv_area_t box;
    box.y1 = a.y1 + BUBBLE_GAP;
    box.y2 = box.y1 + inner_h + 2 * BUBBLE_PY - 1;
    if (b->mine) {
        box.x2 = a.x2 - EDGE;
        box.x1 = box.x2 - w + 1;
    } else {
        box.x1 = a.x1 + EDGE;
        box.x2 = box.x1 + w - 1;
    }
    /* Mine are ink, theirs paper with an ink edge; the last of a run has
     * a square corner on its writer's side. */
    lv_color_t ink = b->mine ? clr->bg : clr->text;
    lv_color_t paper = b->mine ? clr->text : clr->bg;
    draw_box(layer, &box, BUBBLE_R, true, paper, b->mine ? 0 : 1, clr->text);
    if (b->tail) {
        lv_area_t corner = {b->mine ? box.x2 - BUBBLE_R : box.x1, box.y2 - BUBBLE_R,
                            b->mine ? box.x2 : box.x1 + BUBBLE_R, box.y2};
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = paper;
        d.border_color = clr->text;
        d.border_width = b->mine ? 0 : 1;
        d.border_side = (b->mine ? LV_BORDER_SIDE_RIGHT : LV_BORDER_SIDE_LEFT) | LV_BORDER_SIDE_BOTTOM;
        lv_draw_rect(layer, &d, &corner);
    }
    bool chosen = strcmp(b->id, s_ui.chosen) == 0 || key_row() == (int)base->id1;
    if (chosen) {
        lv_area_t ring = {box.x1 - 3, box.y1 - 3, box.x2 + 3, box.y2 + 3};
        draw_box(layer, &ring, BUBBLE_R + 3, false, clr->bg, 2, clr->text);
    }

    int32_t x = box.x1 + BUBBLE_PX, y = box.y1 + BUBBLE_PY;
    int32_t right = box.x2 - BUBBLE_PX;
    if (b->quote[0]) {
        lv_area_t bar = {x, y + 1, x + 2, y + lh - 2};
        draw_box(layer, &bar, 0, true, ink, 0, ink);
        draw_text(layer, b->quote, &s_font, ink, LV_TEXT_ALIGN_LEFT, x + 7, y, right, y + lh);
        y += lh;
    }
    if (b->voice_ms) {
        /* A round Play (Stop while it plays), the waveform, its length. */
        bool playing = strcmp(b->id, s_ui.playing) == 0;
        lv_area_t knob = {x, y + 3, x + 2 * lh - 7, y + 2 * lh - 4};
        draw_box(layer, &knob, LV_RADIUS_CIRCLE, true, ink, 0, ink);
        draw_text(layer, playing ? TC_ICON_STOP : TC_ICON_PLAY, &s_font, paper, LV_TEXT_ALIGN_CENTER, knob.x1 + 1,
                  knob.y1 + (lv_area_get_height(&knob) - lh) / 2, knob.x2, knob.y2);
        tailchat_waveform(b->id, s_ui.store->waves, WAVE_BARS, 9);
        int32_t wx = knob.x2 + 8, wy = y + lh / 2 + 2;
        for (int i = 0; i < WAVE_BARS; i++) {
            int32_t half = s_ui.store->waves[i];
            lv_area_t bar = {wx + i * 4, wy - half, wx + i * 4 + 1, wy + half};
            draw_box(layer, &bar, 0, true, ink, 0, ink);
        }
        draw_text(layer, b->length, &s_font, ink, LV_TEXT_ALIGN_LEFT, wx, y + lh + 2, right, y + 2 * lh + 2);
        draw_text(layer, b->meta, &s_font, ink, LV_TEXT_ALIGN_RIGHT, wx, y + lh + 2, right, y + 2 * lh + 2);
    } else {
        draw_text(layer, b->text, &s_font, ink, LV_TEXT_ALIGN_LEFT, x, y, x + b->wrap_w - 1, y + b->text_h);
        int32_t my = b->meta_line ? y + b->text_h : y + b->text_h - lh;
        draw_text(layer, b->meta, &s_font, ink, LV_TEXT_ALIGN_RIGHT, x, my, right, my + lh);
    }

    if (b->react[0]) {
        int32_t pw = text_width(b->react, &s_font) + 12;
        int32_t px = b->mine ? box.x2 - 10 - pw : box.x1 + 10;
        lv_area_t pill = {px, box.y2 - 4, px + pw - 1, box.y2 - 4 + lh + 3};
        draw_box(layer, &pill, LV_RADIUS_CIRCLE, true, clr->bg, 1, clr->text);
        draw_text(layer, b->react, &s_font, clr->text, LV_TEXT_ALIGN_CENTER, pill.x1, pill.y1 + 2, pill.x2, pill.y2);
    }
}

/* Whether the text has Arabic in it, which runs right to left: its time
 * then goes on a line of its own. */
static bool has_arabic(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0xD8 && c <= 0xDB) return true;
    }
    return false;
}

/* A message's bubble: its text as it wraps, its time beside the last line
 * if it fits there, else under it. */
static void lay_out_bubble(bubble_t *b, const char *text)
{
    int32_t max = bubble_max(), lh = lv_font_get_line_height(&s_font);
    b->cut = tailchat_excerpt(b->text, sizeof b->text, text, TEXT_MAX, TEXT_LINES);
    if (!b->text[0] && !b->voice_ms) snprintf(b->text, sizeof b->text, " ");
    int32_t meta_w = text_width(b->meta, &s_font);
    int32_t quote_w = b->quote[0] ? text_width(b->quote, &s_font) + 7 : 0;
    if (b->voice_ms) {
        b->inner_w = VOICE_W < max ? VOICE_W : max;
        b->wrap_w = b->inner_w;
        b->text_h = lh;
        return;
    }
    lv_point_t full, narrow;
    lv_text_get_size(&full, b->text, &s_font, 0, 0, max, LV_TEXT_FLAG_NONE);
    b->meta_line = false;
    b->wrap_w = max;
    b->inner_w = full.x;
    if (b->meta[0]) {
        lv_text_get_size(&narrow, b->text, &s_font, 0, 0, max - meta_w - META_GAP, LV_TEXT_FLAG_NONE);
        if (narrow.y == full.y && !has_arabic(b->text)) {
            b->wrap_w = max - meta_w - META_GAP;
            b->inner_w = narrow.x + META_GAP + meta_w;
        } else {
            b->meta_line = true;
            if (meta_w > b->inner_w) b->inner_w = meta_w;
        }
    }
    if (quote_w > b->inner_w) b->inner_w = quote_w > max ? max : quote_w;
    b->text_h = full.y;
}

/* The text the table holds for a row: its lines, so its height is the
 * bubble's. */
static const char *bubble_cell_text(const bubble_t *b, char *out, size_t n)
{
    size_t at = 0;
#define ADD(s) at += (size_t)snprintf(out + at, at < n ? n - at : 0, "%s", (s))
    if (b->kind != BUBBLE_MESSAGE) {
        ADD("-");
    } else {
        if (b->quote[0]) ADD("-\n");
        if (b->voice_ms) ADD("-\n-");
        else ADD(b->text);
        if (b->meta_line) ADD("\n-");
        if (b->react[0]) ADD("\n-");
    }
#undef ADD
    if (at >= n) out[n - 1] = '\0';
    return out;
}

static void reaction_text(char *out, size_t n, const tailnet_chat_message_t *m)
{
    out[0] = '\0';
    if (m->react_them[0] && m->react_me[0] && strcmp(m->react_them, m->react_me) == 0) {
        snprintf(out, n, "%s 2", m->react_them);
    } else if (m->react_them[0] && m->react_me[0]) {
        snprintf(out, n, "%s %s", m->react_them, m->react_me);
    } else {
        snprintf(out, n, "%s", m->react_them[0] ? m->react_them : m->react_me);
    }
}

/* The rows of a conversation, or of a request's messages: a heading for
 * each day, the line above what came unread, the bubbles. */
static int build_conversation(bubble_t *rows, const tailnet_chat_contact_t *c)
{
    struct tm now, then, before = {0};
    bool h24;
    bool known = now_tm(&now, &h24);
    int n = tailnet_chat_message_count(c->key);
    if (n > SHOWN_MAX) n = SHOWN_MAX;
    s_ui.messages = n;

    /* The unread line goes above the first of the last `unread` received. */
    int unread_from = n;
    for (int i = n - 1, left = s_ui.unread_at_open; i >= 0 && left > 0; i--) {
        tailnet_chat_message_t m;
        if (tailnet_chat_get_message(c->key, i, &m, NULL, 0) == ESP_OK && !m.mine) {
            unread_from = i;
            left--;
        }
    }

    int count = 0;
    bool before_mine = false;
    uint64_t before_ts = 0;
    for (int i = 0; i < n; i++) {
        tailnet_chat_message_t m;
        if (tailnet_chat_get_message(c->key, i, &m, s_ui.store->text, sizeof s_ui.store->text) != ESP_OK) continue;
        ms_tm(m.ts, &then);
        if (known && (i == 0 || !tailchat_same_day(&before, &then))) {
            bubble_t *d = &rows[count++];
            d->kind = BUBBLE_DAY;
            tailchat_day(&now, &then, d->text, sizeof d->text);
        }
        if (i == unread_from) {
            bubble_t *u = &rows[count++];
            u->kind = BUBBLE_UNREAD;
            snprintf(u->text, sizeof u->text, s_ui.unread_at_open == 1 ? "1 new message" : "%u new messages",
                     (unsigned)s_ui.unread_at_open);
        }
        /* The one before is the last of its run unless this goes on with it. */
        if (count > 0 && i > 0 && rows[count - 1].kind == BUBBLE_MESSAGE &&
            tailchat_joins(before_mine, before_ts, m.mine, m.ts)) {
            rows[count - 1].tail = false;
            rows[count - 1].meta[0] = rows[count - 1].tick == TAILCHAT_TICK_WAITING ? rows[count - 1].meta[0] : '\0';
        }
        bubble_t *b = &rows[count++];
        b->kind = BUBBLE_MESSAGE;
        b->mine = m.mine;
        b->tail = true;
        b->voice_ms = m.voice_ms;
        if (m.voice_ms) tailchat_duration(b->length, sizeof b->length, m.voice_ms);
        b->tick = (uint8_t)tailchat_tick(m.mine, m.state);
        memcpy(b->id, m.id, sizeof b->id);
        char clock[16];
        tailchat_clock(&then, h24, clock, sizeof clock);
        snprintf(b->meta, sizeof b->meta, "%s%s%s", clock, b->tick ? " " : "", tick_icon(b->tick));
        if (m.reply_to[0]) {
            bool quote_mine = false;
            if (tailnet_chat_find_message(c->key, m.reply_to, &quote_mine, s_ui.store->quote,
                                          sizeof s_ui.store->quote) == ESP_OK) {
                snprintf(s_ui.store->line, sizeof s_ui.store->line, "%s: %.800s", quote_mine ? "You" : c->name,
                         s_ui.store->quote);
            } else {
                snprintf(s_ui.store->line, sizeof s_ui.store->line, "An earlier message");
            }
            fit_line(b->quote, sizeof b->quote, s_ui.store->line, &s_font, bubble_max() - 7);
        }
        reaction_text(b->react, sizeof b->react, &m);
        before_mine = m.mine;
        before_ts = m.ts;
        before = then;
        /* The time is laid out with the bubble; a run's last keeps it. */
        lay_out_bubble(b, s_ui.store->text);
    }
    /* A bubble that lost its time to the next of its run is laid out again
     * without it. */
    for (int i = 0; i < count; i++) {
        bubble_t *b = &rows[i];
        if (b->kind == BUBBLE_MESSAGE && !b->tail && !b->meta[0] && !b->voice_ms) {
            char *keep = s_ui.store->quote;
            snprintf(keep, sizeof s_ui.store->quote, "%s", b->text);
            bool cut = b->cut;
            lay_out_bubble(b, keep);
            b->cut = cut;
        }
    }
    if (count == 0) {
        rows[count].kind = BUBBLE_EMPTY;
        snprintf(rows[count++].text, sizeof rows[0].text, "Say hello");
    }
    return count;
}

/* The table's rows from the bubbles, after they changed. */
static void apply_bubbles(int n)
{
    s_ui.rows = n;
    lv_table_set_row_count(s_ui.table, (uint32_t)n);
    char *cell = s_ui.store->text;
    for (int i = 0; i < n; i++) {
        lv_table_set_cell_value(s_ui.table, (uint32_t)i, 0,
                                bubble_cell_text(&s_ui.store->bubbles[i], cell, sizeof s_ui.store->text));
    }
    lv_obj_invalidate(s_ui.table);
}

/* The height of the rows above `row`, as the table lays them out. */
static int32_t rows_above(int row)
{
    int32_t lh = lv_font_get_line_height(&s_font), y = 0;
    for (int i = 0; i < row && i < s_ui.rows; i++) {
        lv_point_t size;
        lv_text_get_size(&size, lv_table_get_cell_value(s_ui.table, (uint32_t)i, 0), &s_font, 0, 0, bubble_max(),
                         LV_TEXT_FLAG_NONE);
        y += (size.y > 0 ? size.y : lh) + 2 * (BUBBLE_GAP + BUBBLE_PY);
    }
    return y;
}

static void scroll_conversation(bool opening)
{
    lv_obj_update_layout(s_ui.body);
    if (opening && s_ui.unread_at_open) {
        /* To the new messages, if they are more than a screen. */
        for (int i = 0; i < s_ui.rows; i++) {
            if (s_ui.store->bubbles[i].kind != BUBBLE_UNREAD) continue;
            int32_t y = rows_above(i) + lv_obj_get_y(s_ui.table);
            int32_t below = lv_obj_get_height(s_ui.table) + lv_obj_get_y(s_ui.table) - y;
            if (below > lv_obj_get_content_height(s_ui.body)) {
                lv_obj_scroll_to_y(s_ui.body, y, LV_ANIM_OFF);
                return;
            }
        }
    }
    lv_obj_scroll_to_y(s_ui.body, LV_COORD_MAX, LV_ANIM_OFF);
}

/* The conversation's messages and what the contact is doing: drawn again
 * only if anything shown changed, leaving the compose field as it is. */
static void fill_conversation(bool opening)
{
    tailnet_chat_contact_t *c = &s_ui.store->contact;
    if (!find_contact(s_ui.key, c)) {
        go_back();
        return;
    }
    set_text(s_ui.title, c->name);
    tailchat_presence_t p = {.typing = c->typing, .pending = c->pending, .waiting = c->waiting > 0,
                             .connected = c->connected, .seen_name = c->seen_name};
    set_text(s_ui.presence, tailchat_presence(&p));

    store_t *st = s_ui.store;
    memset(st->next_bubbles, 0, sizeof st->next_bubbles);
    int n = build_conversation(st->next_bubbles, c);
    bool at_bottom = lv_obj_get_scroll_bottom(s_ui.body) <= 24;
    if (opening || n != s_ui.rows || memcmp(st->bubbles, st->next_bubbles, sizeof st->bubbles[0] * (size_t)n) != 0) {
        memcpy(st->bubbles, st->next_bubbles, sizeof st->bubbles);
        apply_bubbles(n);
        if (opening || at_bottom) scroll_conversation(opening);
    }

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "conversation %s: %d messages; LVGL pool %u KB free, largest block %u KB", c->name,
             s_ui.messages, (unsigned)(mon.free_size / 1024), (unsigned)(mon.free_biggest_size / 1024));
}

/* ---- typing, sending, recording ------------------------------------ */

/* Tell the contact typing stopped, if it was told it had started. */
static void typing_off(void)
{
    if (s_ui.typing && s_ui.conv_key[0]) tailnet_chat_set_typing(s_ui.conv_key, false);
    s_ui.typing = false;
}

/* The composer's button: the microphone while the field is empty, Send
 * (ink) once there is text. Changed only when that does. */
static void show_action(void)
{
    if (!s_ui.action || !s_ui.field) return;
    bool text = lv_textarea_get_text(s_ui.field)[0] != '\0';
    lv_obj_t *l = lv_obj_get_child(s_ui.action, 0);
    const char *icon = text ? TC_ICON_SEND : TC_ICON_MIC;
    if (strcmp(lv_label_get_text(l), icon) == 0) return;
    lv_label_set_text(l, icon);
    if (text) lv_obj_add_style(s_ui.action, &s_styles.tab_on, LV_PART_MAIN);
    else lv_obj_remove_style(s_ui.action, &s_styles.tab_on, LV_PART_MAIN);
}

static void typed_cb(lv_event_t *e)
{
    (void)e;
    uint32_t now = lv_tick_get();
    s_ui.typed = now;
    show_action();
    if (lv_textarea_get_text(s_ui.field)[0] == '\0') {
        typing_off();
        return;
    }
    if (!s_ui.typing || lv_tick_elaps(s_ui.typing_said) >= TYPING_REPEAT_MS) {
        tailnet_chat_set_typing(s_ui.key, true);
        s_ui.typing = true;
        s_ui.typing_said = now;
    }
}

static void show_reply_bar(void)
{
    if (!s_ui.reply_bar) return;
    bool mine = false;
    if (!s_ui.reply_to[0]) {
        set_shown(s_ui.reply_bar, false);
        return;
    }
    if (tailnet_chat_find_message(s_ui.key, s_ui.reply_to, &mine, s_ui.store->quote,
                                  sizeof s_ui.store->quote) != ESP_OK) {
        snprintf(s_ui.store->quote, sizeof s_ui.store->quote, "an earlier message");
    }
    snprintf(s_ui.store->line, sizeof s_ui.store->line, "%s: %.800s", mine ? "You" : lv_label_get_text(s_ui.title),
             s_ui.store->quote);
    char line[160];
    fit_line(line, sizeof line, s_ui.store->line, &s_font, s_ui.width - 2 * EDGE - 2 * BUTTON);
    set_text(lv_obj_get_child(s_ui.reply_bar, 1), line);
    set_shown(s_ui.reply_bar, true);
}

static void send_text(void)
{
    const char *text = lv_textarea_get_text(s_ui.field);
    if (!text[0]) return;
    char id[33] = "";
    esp_err_t err = tailnet_chat_send_text(s_ui.key, text, s_ui.reply_to[0] ? s_ui.reply_to : NULL, id);
    if (err == ESP_ERR_INVALID_SIZE) {
        toast_warn("Too long to send");
        return;
    }
    if (err != ESP_OK) {
        toast_warn("Could not send");
        return;
    }
    ESP_LOGI(TAG, "sent %s%s", id, s_ui.reply_to[0] ? ", a reply" : "");
    typing_off();
    lv_textarea_set_text(s_ui.field, "");
    show_action();
    s_ui.reply_to[0] = '\0';
    show_reply_bar();
}

static void send_cb(lv_event_t *e)
{
    (void)e;
    send_text();
}

/* The composer's button held: with no text, it records; let go, the
 * recording is sent (a tap is not a message). With text, a tap sends. */
static void action_press_cb(lv_event_t *e)
{
    (void)e;
    if (lv_textarea_get_text(s_ui.field)[0]) return;
    const char *why = tailchat_voice_record();
    if (why) {
        toast_warn(why);
        ESP_LOGI(TAG, "no recording: %s", why);
        return;
    }
    snprintf(s_ui.rec_key, sizeof s_ui.rec_key, "%s", s_ui.key);
    snprintf(s_ui.rec_reply, sizeof s_ui.rec_reply, "%s", s_ui.reply_to);
    s_ui.rec_shown_s = UINT32_MAX;
    typing_off();
}

static void action_release_cb(lv_event_t *e)
{
    (void)e;
    uint32_t ms;
    if (tailchat_voice_poll(&ms, NULL) == TAILCHAT_VOICE_RECORDING) tailchat_voice_stop();
}

static void action_click_cb(lv_event_t *e)
{
    (void)e;
    if (lv_textarea_get_text(s_ui.field)[0]) send_text();
}

/* Play voice message `id`, or stop the one playing. */
static void toggle_voice(const char *id)
{
    uint32_t ms;
    const char *playing = "";
    char path[160];
    const char *why = NULL;
    if (tailchat_voice_poll(&ms, &playing) == TAILCHAT_VOICE_PLAYING && strcmp(playing, id) == 0) {
        tailchat_voice_stop();
        return;
    }
    if (tailnet_chat_voice_path(id, path, sizeof path) != ESP_OK) why = "The recording is not on the card";
    else why = tailchat_voice_play(path, id);
    if (why) {
        toast_warn(why);
        ESP_LOGI(TAG, "not playing %s: %s", id, why);
    }
}

static void show_recording(bool on)
{
    if (!s_ui.recording) return;
    set_shown(s_ui.recording, on);
    set_shown(s_ui.field, !on);
}

/* What the recording or the player is doing, twice a second. */
static void voice_tick(void)
{
    uint32_t ms = 0;
    const char *playing = "";
    tailchat_voice_state_t st = tailchat_voice_poll(&ms, &playing);
    if (st == TAILCHAT_VOICE_RECORDED) {
        size_t len;
        uint32_t length;
        const char *why;
        const uint8_t *file = tailchat_voice_take(&len, &length, &why);
        if (file && s_ui.rec_key[0]) {
            char id[33] = "";
            esp_err_t err = tailnet_chat_send_voice(s_ui.rec_key, file, len, length,
                                                    s_ui.rec_reply[0] ? s_ui.rec_reply : NULL, id);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "sent voice %s: %u ms, %u bytes", id, (unsigned)length, (unsigned)len);
                if (strcmp(s_ui.rec_reply, s_ui.reply_to) == 0) {
                    s_ui.reply_to[0] = '\0';
                    show_reply_bar();
                }
            } else {
                toast_warn("Could not send the voice message");
            }
        } else if (why) {
            toast_warn(why);
            ESP_LOGI(TAG, "no voice message: %s", why);
        }
        tailchat_voice_release();
        s_ui.rec_key[0] = '\0';
        show_recording(false);
        if (s_ui.page == PAGE_CONVERSATION && s_ui.table) fill_conversation(false);
    } else if (st == TAILCHAT_VOICE_RECORDING && s_ui.page == PAGE_CONVERSATION && s_ui.recording) {
        if (ms / 1000 != s_ui.rec_shown_s) {
            s_ui.rec_shown_s = ms / 1000;
            char length[16];
            snprintf(s_ui.store->line, sizeof s_ui.store->line, TC_ICON_MIC "  %s of 2:00, let go to send",
                     tailchat_duration(length, sizeof length, ms));
            set_text(s_ui.recording, s_ui.store->line);
            show_recording(true);
        }
    }
    if (strcmp(playing, s_ui.playing) != 0) {
        snprintf(s_ui.playing, sizeof s_ui.playing, "%s", playing);
        ESP_LOGI(TAG, "playing: %s", playing[0] ? playing : "nothing");
        if (s_ui.page == PAGE_CONVERSATION && s_ui.table) lv_obj_invalidate(s_ui.table);
    }
}

/* A call to the contact on the page; the system call screen takes it. */
static void call_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = tailnet_chat_call(s_ui.key);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "calling");
        typing_off();
    } else if (err == ESP_ERR_INVALID_STATE) {
        toast_warn("A call is already up, or chat is off");
    } else {
        toast_warn("Could not call");
    }
}

/* ---- the message sheet ----------------------------------------------- */

static void close_sheet(void)
{
    s_ui.chosen[0] = '\0';
    if (s_ui.sheet) {
        set_shown(s_ui.sheet, false);
        set_shown(s_ui.composer, true);
    }
    if (s_ui.table) lv_obj_invalidate(s_ui.table);
    focus(s_ui.field);
}

static void close_sheet_cb(lv_event_t *e)
{
    (void)e;
    close_sheet();
}

static void reply_to(const char *id)
{
    snprintf(s_ui.reply_to, sizeof s_ui.reply_to, "%s", id);
    show_reply_bar();
    focus(s_ui.field);
}

static void reply_cb(lv_event_t *e)
{
    (void)e;
    char id[33];
    memcpy(id, s_ui.chosen, sizeof id);
    close_sheet();
    reply_to(id);
}

static void cancel_reply_cb(lv_event_t *e)
{
    (void)e;
    s_ui.reply_to[0] = '\0';
    show_reply_bar();
}

static void play_cb(lv_event_t *e)
{
    (void)e;
    char id[33];
    memcpy(id, s_ui.chosen, sizeof id);
    close_sheet();
    toggle_voice(id);
}

static void read_all_cb(lv_event_t *e)
{
    (void)e;
    memcpy(s_ui.read_id, s_ui.chosen, sizeof s_ui.read_id);
    go(PAGE_MESSAGE, s_ui.key);
}

static void react_cb(lv_event_t *e)
{
    intptr_t which = (intptr_t)lv_event_get_user_data(e);
    /* The reaction already ours is taken back. */
    const char *emoji = which < 0 ? "" : tailchat_reactions[which];
    if (tailnet_chat_react(s_ui.key, s_ui.chosen, emoji) == ESP_OK) {
        ESP_LOGI(TAG, "reacted %s", emoji[0] ? tailchat_emoji_label(emoji) : "(removed)");
    }
    close_sheet();
}

static const bubble_t *bubble_of(const char *id)
{
    for (int i = 0; i < s_ui.rows; i++) {
        if (s_ui.store->bubbles[i].kind == BUBBLE_MESSAGE && strcmp(s_ui.store->bubbles[i].id, id) == 0) {
            return &s_ui.store->bubbles[i];
        }
    }
    return NULL;
}

/* A message chosen: the reactions, Reply, and Play or Read all. */
static void open_sheet(const char *id)
{
    const bubble_t *b = bubble_of(id);
    if (!b || !s_ui.sheet) return;
    snprintf(s_ui.chosen, sizeof s_ui.chosen, "%s", id);
    lv_obj_clean(s_ui.sheet);

    tailnet_chat_message_t m = {0};
    int n = tailnet_chat_message_count(s_ui.key);
    for (int i = 0; i < n; i++) {
        if (tailnet_chat_get_message(s_ui.key, i, &m, NULL, 0) == ESP_OK && strcmp(m.id, id) == 0) break;
        m.react_me[0] = '\0';
    }
    lv_obj_t *reacts = plain(s_ui.sheet, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(reacts, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < TAILCHAT_REACTIONS; i++) {
        lv_obj_t *r = icon_button(reacts, tailchat_reactions[i], NULL);
        lv_obj_set_style_text_font(lv_obj_get_child(r, 0), &s_reacts, 0);
        bool ours = m.react_me[0] && strncmp(m.react_me, tailchat_reactions[i], 4) == 0;
        if (ours) lv_obj_add_style(r, &s_styles.tab_on, LV_PART_MAIN);
        lv_obj_add_event_cb(r, react_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(ours ? -1 : i));
    }
    lv_obj_t *row = plain(s_ui.sheet, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_t *first = NULL;
    if (b->voice_ms) {
        first = button(row, strcmp(s_ui.playing, id) == 0 ? TC_ICON_STOP " Stop" : TC_ICON_PLAY " Play", play_cb,
                       false);
    }
    lv_obj_t *reply = button(row, TC_ICON_REPLY " Reply", reply_cb, false);
    if (!first) first = reply;
    if (b->cut) button(row, "Read all", read_all_cb, false);
    lv_obj_t *gap = plain(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(gap, 1);
    lv_obj_set_width(gap, 0);
    icon_button(row, LV_SYMBOL_CLOSE, close_sheet_cb);

    set_shown(s_ui.composer, false);
    set_shown(s_ui.sheet, true);
    lv_obj_invalidate(s_ui.table);
    focus(first);
    ESP_LOGI(TAG, "chose %s", id);
}

/* A bubble tapped: a voice message plays; anything else opens its sheet,
 * as a long press and Enter do; again, it closes. */
static void choose_message(const char *id, bool sheet)
{
    if (strcmp(s_ui.chosen, id) == 0) {
        close_sheet();
        return;
    }
    const bubble_t *b = bubble_of(id);
    if (!b) return;
    if (b->voice_ms && !sheet) {
        ESP_LOGI(TAG, "chose %s", id);
        toggle_voice(id);
        return;
    }
    open_sheet(id);
}

static void bubbles_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (code == LV_EVENT_LONG_PRESSED) {
        /* A slow swipe lasts past a long press: the swipe wins. */
        if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE) return;
        uint32_t row = LV_TABLE_CELL_NONE, col = LV_TABLE_CELL_NONE;
        lv_table_get_selected_cell(s_ui.table, &row, &col);
        if (row < (uint32_t)s_ui.rows && s_ui.store->bubbles[row].kind == BUBBLE_MESSAGE) {
            s_ui.long_pressed = true;
            open_sheet(s_ui.store->bubbles[row].id);
        }
        return;
    }
    bool keys = indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD;
    if (keys && lv_indev_get_key(indev) != LV_KEY_ENTER) {
        lv_obj_invalidate(s_ui.table);
        return;
    }
    if (s_ui.long_pressed) {
        s_ui.long_pressed = false;
        return;
    }
    uint32_t row = LV_TABLE_CELL_NONE, col = LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(s_ui.table, &row, &col);
    if (row >= (uint32_t)s_ui.rows || s_ui.store->bubbles[row].kind != BUBBLE_MESSAGE) return;
    const char *id = s_ui.store->bubbles[row].id;
    lv_dir_t swipe = indev && !keys ? lv_indev_get_gesture_dir(indev) : LV_DIR_NONE;
    if (swipe == LV_DIR_RIGHT) {
        ESP_LOGI(TAG, "swiped to reply to %s", id);
        reply_to(id);
        return;
    }
    if (swipe != LV_DIR_NONE) return;
    choose_message(id, keys);
}

static void contact_page_cb(lv_event_t *e)
{
    (void)e;
    go(PAGE_CONTACT, s_ui.key);
}

static void show_conversation(void)
{
    tailnet_chat_contact_t *c = &s_ui.store->contact;
    if (!find_contact(s_ui.key, c)) {
        go_back();
        return;
    }
    s_ui.chosen[0] = '\0';
    s_ui.reply_to[0] = '\0';
    s_ui.unread_at_open = c->unread;
    memcpy(s_ui.conv_key, c->key, sizeof s_ui.conv_key);

    lv_obj_t *h = header_row();
    icon_button(h, LV_SYMBOL_LEFT, back_button_cb);
    lv_obj_t *who = lv_obj_create(h);
    lv_obj_remove_style_all(who);
    lv_obj_set_flex_flow(who, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(who, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(who, 8, 0);
    lv_obj_set_height(who, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(who, 1);
    lv_obj_set_width(who, 0);
    lv_obj_remove_flag(who, LV_OBJ_FLAG_SCROLLABLE);
    /* The name opens the contact's page, as in any chat app. */
    lv_obj_add_flag(who, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(who, &s_styles.focus_ring, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(who, contact_page_cb, LV_EVENT_CLICKED, NULL);
    join_group(who);
    char mono[12];
    lv_obj_t *av = lv_label_create(who);
    lv_obj_add_style(av, &s_styles.avatar, LV_PART_MAIN);
    lv_obj_add_style(av, &s_styles.center, LV_PART_MAIN);
    lv_obj_set_size(av, 30, 30);
    lv_obj_set_style_pad_top(av, 6, 0);
    lv_label_set_text(av, tailchat_initials(c->name, mono, sizeof mono));
    if (s_ui.tiny) lv_obj_add_flag(av, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *col = plain(who, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_width(col, 0);
    s_ui.title = line_label(col, c->name, &s_styles.title);
    lv_obj_set_style_text_font(s_ui.title, &s_font_18, 0);
    s_ui.presence = line_label(col, "", &s_styles.subtitle);
    if (s_ui.tiny) lv_obj_add_flag(s_ui.presence, LV_OBJ_FLAG_HIDDEN);
    icon_button(h, LV_SYMBOL_CALL, call_cb);

    s_ui.table = make_table(draw_bubble, bubbles_cb, bubble_side(), bubble_side(), BUBBLE_GAP + BUBBLE_PY);
    lv_obj_add_event_cb(s_ui.table, bubbles_cb, LV_EVENT_LONG_PRESSED, NULL);
    /* Its swipes are its own: a swipe right replies. */
    lv_obj_remove_flag(s_ui.table, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_ui.reply_bar = plain(s_ui.footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.reply_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_style(s_ui.reply_bar, &s_styles.warn, LV_PART_MAIN);
    lv_obj_set_style_margin_left(s_ui.reply_bar, 2, 0);
    lv_obj_t *icon = lv_label_create(s_ui.reply_bar);
    lv_label_set_text(icon, TC_ICON_REPLY " ");
    lv_obj_t *quote = line_label(s_ui.reply_bar, "", NULL);
    lv_obj_set_flex_grow(quote, 1);
    lv_obj_set_width(quote, 0);
    icon_button(s_ui.reply_bar, LV_SYMBOL_CLOSE, cancel_reply_cb);
    set_shown(s_ui.reply_bar, false);

    s_ui.sheet = plain(s_ui.footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.sheet, 4, 0);
    set_shown(s_ui.sheet, false);

    s_ui.composer = plain(s_ui.footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.composer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_ui.composer, 6, 0);
    s_ui.field = lv_textarea_create(s_ui.composer);
    lv_obj_add_style(s_ui.field, &s_styles.field, LV_PART_MAIN);
    lv_textarea_set_one_line(s_ui.field, true);
    lv_textarea_set_max_length(s_ui.field, 1024);
    lv_textarea_set_placeholder_text(s_ui.field, "Message");
    lv_obj_set_flex_grow(s_ui.field, 1);
    lv_obj_add_event_cb(s_ui.field, send_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_ui.field, typed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    join_group(s_ui.field);
    s_ui.recording = line_label(s_ui.composer, "", NULL);
    lv_obj_set_flex_grow(s_ui.recording, 1);
    lv_obj_set_width(s_ui.recording, 0);
    lv_obj_set_style_pad_left(s_ui.recording, 6, 0);
    set_shown(s_ui.recording, false);
    /* Held with no text, it records a voice message; let go, it is sent. */
    s_ui.action = icon_button(s_ui.composer, TC_ICON_MIC, NULL);
    lv_obj_set_style_border_width(s_ui.action, 1, 0);
    lv_obj_add_event_cb(s_ui.action, action_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ui.action, action_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_ui.action, action_release_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(s_ui.action, action_click_cb, LV_EVENT_CLICKED, NULL);

    tailnet_chat_set_viewing(c->key);
    fill_conversation(true);
    focus(s_ui.field);
}

/* The whole of a message too long for its bubble. */
static void show_message(void)
{
    tailnet_chat_contact_t *c = &s_ui.store->contact;
    bool mine = false;
    if (!find_contact(s_ui.key, c) ||
        tailnet_chat_find_message(s_ui.key, s_ui.read_id, &mine, s_ui.store->text, sizeof s_ui.store->text) !=
            ESP_OK) {
        go_back();
        return;
    }
    page_header(mine ? "You" : c->name, NULL);
    lv_obj_t *card = plain(s_ui.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(card, &s_styles.card, LV_PART_MAIN);
    lv_obj_set_width(card, s_ui.width - 2 * EDGE);
    lv_obj_set_style_margin_top(card, 8, 0);
    lv_obj_t *l = label(card, "", NULL);
    lv_label_set_text_static(l, s_ui.store->text);
}

/* ------------------------------------------------------------------ */
/* A contact                                                            */
/* ------------------------------------------------------------------ */

static void message_cb(lv_event_t *e)
{
    (void)e;
    go(PAGE_CONVERSATION, s_ui.key);
}

static void rename_cb(lv_event_t *e)
{
    (void)e;
    const char *name = lv_textarea_get_text(s_ui.field);
    if (tailnet_chat_rename(s_ui.key, name) == ESP_OK) {
        ESP_LOGI(TAG, "renamed to %s", name);
        toast_info("Renamed");
        set_text(s_ui.title, name);
    } else {
        toast_warn("A name needs a letter in it");
    }
}

static void new_key_cb(lv_event_t *e)
{
    (void)e;
    if (tailnet_chat_accept_new_key(s_ui.key) == ESP_OK) {
        ESP_LOGI(TAG, "accepted the new key");
        toast_info("New key accepted");
        go_back();
    }
}

static void remove_cb(lv_event_t *e)
{
    (void)e;
    if (!s_ui.confirm_remove) {
        s_ui.confirm_remove = true;
        set_text(lv_obj_get_child(s_ui.remove, 0), LV_SYMBOL_TRASH "  Press again to remove");
        return;
    }
    if (tailnet_chat_remove(s_ui.key) == ESP_OK) {
        ESP_LOGI(TAG, "removed a contact");
        toast_info("Removed; it will have to ask again");
    }
    go(PAGE_CONTACTS, NULL);
}

/* A round button with its word under it, as a profile's are. */
static lv_obj_t *round_action(lv_obj_t *parent, const char *icon, const char *word, lv_event_cb_t cb)
{
    lv_obj_t *col = plain(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_t *b = icon_button(col, icon, cb);
    lv_obj_set_size(b, 44, 44);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_t *l = lv_label_create(col);
    lv_label_set_text(l, word);
    return b;
}

/* A monogram as a widget, for a page's head. */
static void big_avatar(lv_obj_t *parent, const char *name, bool solid)
{
    char mono[12];
    lv_obj_t *av = lv_label_create(parent);
    lv_obj_add_style(av, &s_styles.avatar, LV_PART_MAIN);
    lv_obj_add_style(av, &s_styles.center, LV_PART_MAIN);
    if (!solid) {
        lv_obj_set_style_bg_opa(av, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(av, theme_get_colors()->text, 0);
    }
    lv_obj_set_style_text_font(av, &s_font_22, 0);
    lv_obj_set_size(av, 56, 56);
    lv_obj_set_style_pad_top(av, 15, 0);
    lv_label_set_text(av, tailchat_initials(name, mono, sizeof mono));
}

static lv_obj_t *head(void)
{
    lv_obj_t *col = plain(s_ui.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(col, s_ui.compact ? 4 : 10, 0);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_set_style_pad_hor(col, EDGE, 0);
    return col;
}

static void show_contact(void)
{
    tailnet_chat_contact_t *c = &s_ui.store->contact;
    if (!find_contact(s_ui.key, c)) {
        go_back();
        return;
    }
    page_header("Contact", NULL);
    lv_obj_t *top = head();
    big_avatar(top, c->name, !c->pending);
    s_ui.title = line_label(top, c->name, &s_styles.title);
    lv_obj_add_style(s_ui.title, &s_styles.center, LV_PART_MAIN);
    tailchat_presence_t p = {.pending = c->pending, .connected = c->connected, .seen_name = c->seen_name};
    s_ui.presence = line_label(top, tailchat_presence(&p), &s_styles.subtitle);
    lv_obj_add_style(s_ui.presence, &s_styles.center, LV_PART_MAIN);

    lv_obj_t *actions = plain(s_ui.body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(actions, 28, 0);
    lv_obj_set_style_pad_ver(actions, 4, 0);
    lv_obj_t *message = round_action(actions, TC_ICON_CHAT, "Message", message_cb);
    round_action(actions, LV_SYMBOL_CALL, "Call", call_cb);

    if (c->flagged) {
        snprintf(s_ui.store->line, sizeof s_ui.store->line,
                 LV_SYMBOL_WARNING " Another key came under this name or address and was refused:\n%s\n"
                 "Accept it only if you know why the key changed, such as a reinstalled app.",
                 c->flagged_fingerprint);
        label(s_ui.body, s_ui.store->line, &s_styles.warn);
        lv_obj_t *row = plain(s_ui.body, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_hor(row, EDGE, 0);
        button(row, "Accept new key", new_key_cb, false);
    }

    lv_obj_t *card = plain(s_ui.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(card, &s_styles.card, LV_PART_MAIN);
    lv_obj_set_width(card, s_ui.width - 2 * EDGE);
    const char *answer = tailchat_answer_text(c->accepted_by_them);
    snprintf(s_ui.store->line, sizeof s_ui.store->line, TC_ICON_KEY "  Fingerprint\n%s\n%s%s%s%s",
             c->fingerprint, c->seen_name[0] ? "Seen as " : "", c->seen_name, answer[0] && c->seen_name[0] ? "\n" : "",
             answer);
    label(card, s_ui.store->line, NULL);

    label(s_ui.body, "Name", &s_styles.section);
    lv_obj_t *row = plain(s_ui.body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, EDGE, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    s_ui.field = lv_textarea_create(row);
    lv_obj_add_style(s_ui.field, &s_styles.field, LV_PART_MAIN);
    lv_textarea_set_one_line(s_ui.field, true);
    lv_textarea_set_max_length(s_ui.field, 64);
    lv_textarea_set_text(s_ui.field, c->name);
    lv_obj_set_flex_grow(s_ui.field, 1);
    lv_obj_add_event_cb(s_ui.field, rename_cb, LV_EVENT_READY, NULL);
    join_group(s_ui.field);
    button(row, "Rename", rename_cb, false);

    lv_obj_t *end = plain(s_ui.body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_hor(end, EDGE, 0);
    lv_obj_set_style_pad_top(end, 6, 0);
    s_ui.remove = button(end, LV_SYMBOL_TRASH "  Remove contact", remove_cb, false);
    lv_obj_set_flex_grow(s_ui.remove, 1);
    focus(message);
    ESP_LOGI(TAG, "contact %s %s%s", c->name, c->fingerprint, c->flagged ? ", flagged" : "");
}

/* ------------------------------------------------------------------ */
/* A contact request                                                    */
/* ------------------------------------------------------------------ */

static void accept_cb(lv_event_t *e)
{
    (void)e;
    if (tailnet_chat_accept(s_ui.key) == ESP_OK) {
        ESP_LOGI(TAG, "accepted the request");
        toast_info("Accepted");
        go_instead(PAGE_CONVERSATION, s_ui.key);
    } else {
        toast_warn("Could not accept: too many contacts");
        go_back();
    }
}

static void decline_cb(lv_event_t *e)
{
    (void)e;
    if (tailnet_chat_decline(s_ui.key) == ESP_OK) {
        ESP_LOGI(TAG, "declined the request");
        toast_info("Declined");
    }
    go_back();
}

static void show_request(void)
{
    tailnet_chat_request_t *r = &s_ui.store->request;
    if (!find_request(s_ui.key, r)) {
        go_back();
        return;
    }
    page_header("Contact request", NULL);
    lv_obj_t *top = head();
    big_avatar(top, r->name, false);
    lv_obj_t *name = line_label(top, r->name, &s_styles.title);
    lv_obj_add_style(name, &s_styles.center, LV_PART_MAIN);
    label(top, "wants to chat with you", &s_styles.center);

    lv_obj_t *card = plain(s_ui.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(card, &s_styles.card, LV_PART_MAIN);
    lv_obj_set_width(card, s_ui.width - 2 * EDGE);
    snprintf(s_ui.store->line, sizeof s_ui.store->line,
             TC_ICON_KEY "  %s\nCheck it with them before you accept: their key is pinned to it.", r->fingerprint);
    label(card, s_ui.store->line, NULL);

    /* Everything it held, oldest first, as its bubbles; the kernel keeps
     * the latest 20. */
    label(s_ui.body, r->held == 1 ? "Its message" : "Its messages", &s_styles.section);
    s_ui.table = make_table(draw_bubble, NULL, bubble_side(), bubble_side(), BUBBLE_GAP + BUBBLE_PY);
    int held = r->held > 0 ? r->held : 1;
    bubble_t *rows = s_ui.store->bubbles;
    memset(rows, 0, sizeof s_ui.store->bubbles);
    int n = 0;
    for (int i = 0; i < held && n < CONV_ROWS; i++) {
        if (tailnet_chat_get_held(r->key, i, s_ui.store->text, sizeof s_ui.store->text) != ESP_OK) {
            snprintf(s_ui.store->text, sizeof s_ui.store->text, "%s", i == 0 ? r->preview : "");
        }
        bubble_t *b = &rows[n++];
        b->kind = BUBBLE_MESSAGE;
        b->tail = i == held - 1;
        snprintf(b->id, sizeof b->id, "held-%d", i);
        lay_out_bubble(b, s_ui.store->text);
    }
    apply_bubbles(n);

    lv_obj_t *bar = plain(s_ui.footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_t *decline = button(bar, "Decline", decline_cb, false);
    lv_obj_set_flex_grow(decline, 1);
    lv_obj_t *accept = button(bar, "Accept", accept_cb, true);
    lv_obj_set_flex_grow(accept, 1);
    focus(accept);
    ESP_LOGI(TAG, "request %s %s: %u held", r->name, r->fingerprint, (unsigned)r->held);
}

/* ------------------------------------------------------------------ */
/* Drawing the page                                                     */
/* ------------------------------------------------------------------ */

/* The conversation leaves the screen: its messages alert again. */
static void leave_conversation(void)
{
    typing_off();
    tailnet_chat_set_viewing(NULL);
    s_ui.conv_key[0] = '\0';
}

static void show_page(void)
{
    s_ui.generation = tailnet_chat_generation();
    if (s_ui.conv_key[0]) leave_conversation();
    lv_obj_clean(s_ui.header);
    set_shown(s_ui.header, true);
    lv_obj_clean(s_ui.body);
    lv_obj_clean(s_ui.footer);
    lv_obj_scroll_to_y(s_ui.body, 0, LV_ANIM_OFF);
    s_empty = NULL;
    s_ui.table = NULL;
    s_ui.field = NULL;
    s_ui.field2 = NULL;
    s_ui.remove = NULL;
    s_ui.title = NULL;
    s_ui.presence = NULL;
    s_ui.composer = NULL;
    s_ui.action = NULL;
    s_ui.recording = NULL;
    s_ui.sheet = NULL;
    s_ui.reply_bar = NULL;
    s_ui.look_status = NULL;
    s_ui.look_again = NULL;
    memset(s_ui.tab_badges, 0, sizeof s_ui.tab_badges);
    s_ui.chosen[0] = '\0';
    s_ui.rows = 0;
    switch (s_ui.page) {
    case PAGE_CHATS:        tab_page("Chats"); break;
    case PAGE_CONTACTS:     tab_page("Contacts"); break;
    case PAGE_REQUESTS:     tab_page("Requests"); break;
    case PAGE_CONVERSATION: show_conversation(); break;
    case PAGE_CONTACT:      show_contact(); break;
    case PAGE_REQUEST:      show_request(); break;
    case PAGE_ADD:          show_add(); break;
    case PAGE_FIND:         show_find(); break;
    case PAGE_SETTINGS:     show_settings(); break;
    case PAGE_MESSAGE:      show_message(); break;
    }
    set_shown(s_ui.footer, lv_obj_get_child_count(s_ui.footer) > 0);
    show_status();
}

/* Twice a second: draw again only if anything shown changed, and never
 * under the user's typing. */
static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_ui.typing && lv_tick_elaps(s_ui.typed) >= TYPING_STOP_MS) typing_off();
    voice_tick();
    if (tailnet_chat_generation() == s_ui.generation) return;
    s_ui.generation = tailnet_chat_generation();
    show_status();
    switch (s_ui.page) {
    case PAGE_CHATS:
    case PAGE_CONTACTS:
    case PAGE_REQUESTS:
    case PAGE_SETTINGS:
        refresh_list(false);
        break;
    case PAGE_FIND:
        show_look();
        refresh_list(false);
        break;
    case PAGE_CONVERSATION:
        if (s_ui.table) fill_conversation(false);
        break;
    case PAGE_CONTACT: {
        tailnet_chat_contact_t *c = &s_ui.store->contact;
        if (s_ui.presence && find_contact(s_ui.key, c)) {
            tailchat_presence_t p = {.pending = c->pending, .connected = c->connected, .seen_name = c->seen_name};
            set_text(s_ui.presence, tailchat_presence(&p));
        }
        break;
    }
    case PAGE_REQUEST:
        if (!find_request(s_ui.key, &s_ui.store->request)) go_back();
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Making and unmaking                                                  */
/* ------------------------------------------------------------------ */

esp_err_t tailchat_ui_create(lv_obj_t *parent)
{
    if (!parent) parent = lv_screen_active();
    memset(&s_ui, 0, sizeof s_ui);
    s_empty = NULL;
    tailchat_voice_release();   /* a recording finished after the app was left */
    s_ui.store = heap_caps_malloc(sizeof *s_ui.store, MALLOC_CAP_SPIRAM);
    if (!s_ui.store) s_ui.store = malloc(sizeof *s_ui.store);
    if (!s_ui.store) return ESP_ERR_NO_MEM;
    memset(s_ui.store, 0, sizeof *s_ui.store);

    const hal_registry_t *reg = hal_get_registry();
    s_ui.epaper = reg && reg->display && reg->display->type == HAL_DISPLAY_TYPE_EPAPER;
    lv_obj_update_layout(parent);
    s_ui.compact = lv_obj_get_content_height(parent) < 240;
    s_ui.tiny = lv_obj_get_content_height(parent) < 160;
    fonts_init();
    styles_init();

    s_ui.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.root);
    lv_obj_add_style(s_ui.root, &s_styles.page, LV_PART_MAIN);
    lv_obj_set_size(s_ui.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_ui.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.header = plain(s_ui.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(s_ui.header, &s_styles.header, LV_PART_MAIN);

    s_ui.note = plain(s_ui.root, LV_FLEX_FLOW_ROW);
    lv_obj_add_style(s_ui.note, &s_styles.banner, LV_PART_MAIN);
    lv_obj_set_width(s_ui.note, LV_PCT(100));
    lv_obj_t *why = label(s_ui.note, "", NULL);
    lv_obj_set_flex_grow(why, 1);
    lv_obj_set_width(why, 0);
    set_shown(s_ui.note, false);

    s_ui.body = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(s_ui.body);
    lv_obj_add_style(s_ui.body, &s_styles.body, LV_PART_MAIN);
    lv_obj_set_width(s_ui.body, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.body, 1);
    lv_obj_set_flex_flow(s_ui.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_ui.body, LV_DIR_VER);

    s_ui.footer = plain(s_ui.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(s_ui.footer, &s_styles.footer, LV_PART_MAIN);

    lv_obj_update_layout(s_ui.root);
    s_ui.width = lv_obj_get_content_width(s_ui.body);
    if (s_ui.width <= 0) s_ui.width = 240;

    ui_manager_set_back_handler(s_ui.root, back_cb);
    s_ui.page = PAGE_CHATS;
    show_page();
    s_ui.timer = lv_timer_create(timer_cb, 500, NULL);

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "LVGL pool %u KB free, largest block %u KB", (unsigned)(mon.free_size / 1024),
             (unsigned)(mon.free_biggest_size / 1024));
    return ESP_OK;
}

void tailchat_ui_destroy(void)
{
    /* Leaving the app stops a voice message playing, and drops one being
     * recorded: nothing would send it. */
    tailchat_voice_stop();
    tailchat_voice_release();
    if (s_ui.conv_key[0]) {
        leave_conversation();
        request_full_refresh();
    }
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    lv_async_call_cancel(go_async, NULL);
    lv_async_call_cancel(show_page_async, NULL);
    if (s_ui.root) lv_obj_delete(s_ui.root);
    free(s_ui.store);
    memset(&s_ui, 0, sizeof s_ui);
    s_empty = NULL;
    styles_reset();
}
