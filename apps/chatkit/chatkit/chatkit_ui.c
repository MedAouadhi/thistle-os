/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — a chat app's screens, in Chat's look (chatkit.h)
 *
 *   ┌──────────────────────────────┐
 *   │ SMS                      [⚙] │  the app's name; Settings
 *   │ ⚠ The modem is not ready.    │  only while it cannot send
 *   │ (B) Ben Müller        23:18  │  one lv_table: a monogram, the name,
 *   │     ✓ See you then        ②  │  when, the latest and the unread
 *   ├──────────────────────────────┤
 *   │ [Chats 2]        Contacts    │  the tabs, Chats counting the unread
 *   └──────────────────────────────┘
 *
 *   ┌──────────────────────────────┐
 *   │ <  (B) Ben Müller       [☎]  │  back, the conversation, a call
 *   │        +49 160 9876543       │  the app's line under it
 *   │          ( Yesterday )       │  a heading for each day
 *   │ ╭──────────────╮             │  theirs outlined on the left
 *   │ │ Are you com… │             │
 *   │ ╰──────────────╯             │
 *   │      ╭─────────────────────╮ │  mine ink on the right, with the
 *   │      │ Yes! 10:02 ✓        │ │  time and what became of it
 *   │      ╰─────────────────────╯ │
 *   │ ( Message            ) [➤]   │
 *   └──────────────────────────────┘
 *
 * Chat's own screens (tailchat_ui.c) are the model: the same styles, fonts
 * and pictures (tailchat_icons_*, tailchat_emoji_*), the same table-drawn
 * lists and bubbles, the same rule that a page is drawn again only when
 * what it shows has changed. A group conversation (the mesh's Public
 * channel) names the writer over the first bubble of each run of theirs.
 * A bubble cut short opens the whole message when tapped.
 */
#include "chatkit/chatkit.h"

#include "tailchat/tailchat_core.h"
#include "tailchat/tailchat_icons.h"

#include "thistle/msg_alert.h"
#include "thistle/msg_log.h"
#include "thistle/time_manager.h"
#include "hal/board.h"
#include "ui/manager.h"
#include "ui/theme.h"
#include "ui/toast.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "chatkit";

LV_FONT_DECLARE(tailchat_icons_14)
LV_FONT_DECLARE(tailchat_icons_14_mono)
LV_FONT_DECLARE(tailchat_icons_20)
LV_FONT_DECLARE(tailchat_icons_20_mono)
LV_FONT_DECLARE(tailchat_emoji_14)
LV_FONT_DECLARE(tailchat_emoji_14_mono)
LV_FONT_DECLARE(reader_font_16)
LV_FONT_DECLARE(reader_font_16_mono)

#define CONV_ROWS  (CHATKIT_HISTORY * 2 + 1)  /* each message, a day heading before it, the unread line */
#define TEXT_MAX   200  /* characters of a message in its bubble; the message page has the rest */
#define TEXT_LINES 12
#define TEXT_BYTES 720

/* Sizes, in pixels, as Chat's. */
#define EDGE        8
#define AVATAR      36
#define ROW_TEXT_X  (10 + AVATAR + 10)
#define ROW_PAD     10
#define BUBBLE_GAP  3
#define BUBBLE_PX   9
#define BUBBLE_PY   5
#define BUBBLE_R    12
#define BUBBLE_AWAY 40
#define META_GAP    8
#define BUTTON      34

typedef enum {
    PAGE_CHATS,
    PAGE_CONTACTS,
    PAGE_CONVERSATION,
    PAGE_SETTINGS,
    PAGE_MESSAGE,
} page_t;

#define TABS 2

typedef enum {
    BUBBLE_DAY,
    BUBBLE_UNREAD,
    BUBBLE_MESSAGE,
    BUBBLE_EMPTY,
} bubble_kind_t;

typedef struct {
    uint8_t  kind;       /* bubble_kind_t */
    uint8_t  tick;       /* chatkit_status_t, the user's own */
    bool     mine;
    bool     tail;       /* the last of its run: a square corner, and its time */
    bool     meta_line;  /* its time on a line of its own */
    bool     cut;        /* longer than shown: the message page has it whole */
    int16_t  inner_w;
    int16_t  wrap_w;
    int16_t  text_h;
    uint32_t number;     /* which of the conversation's messages, counted from its first ever */
    char     who[64];    /* a group's writer, fitted to one line, over the first of a run */
    char     meta[24];   /* "14:05" and the tick */
    char     text[TEXT_BYTES];
} bubble_t;

/* What the pages read, in PSRAM while the app is open. */
typedef struct {
    chatkit_row_t   rows[CHATKIT_ROWS];
    chatkit_row_t   next_rows[CHATKIT_ROWS];
    bubble_t        bubbles[CONV_ROWS];
    bubble_t        next_bubbles[CONV_ROWS];
    chatkit_conv_t *order[CHATKIT_CONVS];
    char            line[900];
    char            cell[TEXT_BYTES + 16];
    char            whole[CHATKIT_TEXT];   /* the message page's */
} mem_t;

typedef struct {
    page_t page;
    char   key[CHATKIT_ADDR];
} place_t;

static struct {
    const chatkit_app_t *app;
    lv_obj_t   *root;
    lv_obj_t   *header;
    lv_obj_t   *note;
    lv_obj_t   *body;
    lv_obj_t   *footer;
    lv_obj_t   *table;
    lv_obj_t   *empty;
    lv_obj_t   *tab_badge;
    lv_obj_t   *field;      /* the conversation's compose field, or the contacts' search */
    lv_obj_t   *title;
    lv_obj_t   *subtitle;
    lv_timer_t *timer;
    mem_t      *mem;
    page_t      page;
    char        key[CHATKIT_ADDR];  /* the page's conversation */
    place_t     stack[4];
    int         depth;
    place_t     next;
    bool        next_push;
    int         rows;
    bool        in_conv;            /* a conversation is on screen */
    uint32_t    generation;
    uint32_t    contacts_seq;
    uint32_t    reported_unread;
    uint16_t    unread_at_open;
    uint32_t    read_number;        /* the message the message page shows */
    char        query[64];
    bool        epaper;
    bool        compact;
    bool        tiny;
    int32_t     width;
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
    lv_style_t avatar;
    lv_style_t center;
} s_styles;
static bool s_styles_made;

static lv_font_t s_icons;    /* tailchat_icons_14 */
static lv_font_t s_emoji;    /* tailchat_emoji_14 */
static lv_font_t s_latin;    /* the Reader's DejaVu 16: the accents Montserrat lacks, "Müller" */
static lv_font_t s_font;     /* Montserrat 14, then s_icons, then s_emoji, then s_latin */
static lv_font_t s_font_18;
static lv_font_t s_font_22;
static lv_font_t s_buttons;  /* tailchat_icons_20, then Montserrat 18 for LV_SYMBOL_* */

/* ------------------------------------------------------------------ */
/* Fonts and styles, as Chat's                                          */
/* ------------------------------------------------------------------ */

static void fonts_init(void)
{
    bool mono = s_ui.epaper;
    s_latin = mono ? reader_font_16_mono : reader_font_16;
    s_latin.fallback = NULL;
    s_emoji = mono ? tailchat_emoji_14_mono : tailchat_emoji_14;
    s_emoji.fallback = &s_latin;
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
    if (label && strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void set_shown(lv_obj_t *obj, bool shown)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == !shown) return;
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

/* A label held to one line of its font: "..." cuts it only at a fixed
 * height, and it wraps at the content's. Again after its font changes. */
static void keep_one_line(lv_obj_t *l)
{
    lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, LV_PART_MAIN)));
}

static lv_obj_t *line_label(lv_obj_t *parent, const char *text, lv_style_t *style)
{
    lv_obj_t *l = label(parent, text, style);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    keep_one_line(l);
    return l;
}

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
    size_t len = strlen(out);
    size_t lo = 0, hi = len;
    char *probe = s_ui.mem->line;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        while (mid > 0 && ((unsigned char)out[mid] & 0xC0) == 0x80) mid--;
        if (mid <= lo) {
            hi = lo;
            break;
        }
        snprintf(probe, sizeof s_ui.mem->line, "%.*s...", (int)mid, out);
        if (text_width(probe, font) <= max_w) lo = mid;
        else hi = mid - 1;
    }
    while (lo > 0 && out[lo - 1] == ' ') lo--;
    if (lo + 4 > n) lo = n > 4 ? n - 4 : 0;
    memcpy(out + lo, "...", 4);
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

/* Now, in seconds since 1970, or 0 while the clock is not set. */
static uint64_t now_s(void)
{
    struct tm now;
    bool h24;
    return now_tm(&now, &h24) ? (uint64_t)time(NULL) : 0;
}

static void s_tm(uint64_t s, struct tm *out)
{
    time_t t = (time_t)s;
    localtime_r(&t, out);
}

static const char *tick_icon(uint8_t tick)
{
    switch (tick) {
    case CHATKIT_PENDING:   return TC_ICON_CLOCK;
    case CHATKIT_SENT:      return TC_ICON_TICK;
    case CHATKIT_DELIVERED: return TC_ICON_TICKS;
    case CHATKIT_FAILED:    return LV_SYMBOL_WARNING;
    default:                return "";
    }
}

static chatkit_store_t *store(void)
{
    return s_ui.app ? s_ui.app->store : NULL;
}

static chatkit_conv_t *conv_on_page(void)
{
    return store() ? chatkit_find(store(), s_ui.key) : NULL;
}

/* What the app holds unread, for the status bar, when it changes. */
static void report_unread(void)
{
    if (!s_ui.app || !s_ui.app->held_unread) return;
    uint32_t n = chatkit_unread(store());
    if (n == s_ui.reported_unread) return;
    s_ui.reported_unread = n;
    s_ui.app->held_unread(n);
}

/* ------------------------------------------------------------------ */
/* Going between pages                                                  */
/* ------------------------------------------------------------------ */

static void show_page(void);

static bool is_tab(page_t page)
{
    return page <= PAGE_CONTACTS;
}

static void request_full_refresh(void)
{
    const hal_registry_t *reg = hal_get_registry();
    if (!s_ui.epaper || !reg || !reg->display || !reg->display->set_refresh_mode) return;
    reg->display->set_refresh_mode(HAL_DISPLAY_REFRESH_FULL);
}

static void go_async(void *arg)
{
    (void)arg;
    if (!s_ui.root) return;
    if (is_tab(s_ui.next.page)) {
        s_ui.depth = 0;
    } else if (s_ui.next_push) {
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
    show_page();
}

static void go_to(page_t page, const char *key, bool push)
{
    s_ui.next.page = page;
    chatkit_copy(s_ui.next.key, sizeof s_ui.next.key, key ? key : "");
    s_ui.next_push = push;
    lv_async_call(go_async, NULL);
}

static void go(page_t page, const char *key)
{
    go_to(page, key, true);
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

static bool back_cb(void)
{
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

void chatkit_open(const char *conv, const char *title)
{
    chatkit_store_t *st = store();
    if (!st || !s_ui.root) return;
    chatkit_conv_t *c = chatkit_ensure(st, conv, title, false);
    if (!c) {
        toast_warn("No room for another conversation");
        return;
    }
    ESP_LOGI(TAG, "open %s", c->title[0] ? c->title : c->conv);
    go(PAGE_CONVERSATION, c->conv);
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

static void tab_header(const char *title)
{
    lv_obj_t *h = header_row();
    lv_obj_set_style_pad_left(h, EDGE, 0);
    lv_obj_t *t = line_label(h, title, &s_styles.title);
    lv_obj_set_flex_grow(t, 1);
    icon_button(h, LV_SYMBOL_SETTINGS, settings_cb);
}

static void page_header(const char *title)
{
    lv_obj_t *h = header_row();
    icon_button(h, LV_SYMBOL_LEFT, back_button_cb);
    s_ui.title = line_label(h, title, &s_styles.title);
    lv_obj_set_flex_grow(s_ui.title, 1);
    lv_obj_set_width(s_ui.title, 0);
    lv_obj_set_style_text_font(s_ui.title, &s_font_18, 0);
    keep_one_line(s_ui.title);
}

/* Why the app cannot send, over the lists and the conversation; the count
 * on Chats. */
static void show_status(void)
{
    const char *why = s_ui.app->why_not ? s_ui.app->why_not() : NULL;
    lv_obj_t *l = lv_obj_get_child(s_ui.note, 0);
    char text[160];
    snprintf(text, sizeof text, "%s%s", why ? LV_SYMBOL_WARNING "  " : "", why ? why : "");
    if (why && strcmp(lv_label_get_text(l), text) != 0) ESP_LOGI(TAG, "%s: unavailable: %s", s_ui.app->tag, why);
    bool here = is_tab(s_ui.page) || s_ui.page == PAGE_CONVERSATION;
    set_text(l, text);
    set_shown(s_ui.note, why && here);

    if (s_ui.tab_badge) {
        uint32_t n = chatkit_unread(store());
        char count[12];
        snprintf(count, sizeof count, n > 99 ? "99+" : "%u", (unsigned)n);
        set_text(s_ui.tab_badge, count);
        set_shown(s_ui.tab_badge, n > 0);
    }
}

static void tab_bar(void)
{
    const char *names[TABS] = {"Chats", s_ui.app->contacts_tab};
    const char *icons[TABS] = {s_ui.app->chats_icon, s_ui.app->contacts_icon};
    lv_obj_t *bar = plain(s_ui.footer, LV_FLEX_FLOW_ROW);
    lv_obj_t *chats_pic = NULL;
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
        lv_obj_remove_style_all(pic);
        lv_obj_add_style(pic, &s_styles.tab_pic, LV_PART_MAIN);
        if (on) lv_obj_add_style(pic, &s_styles.tab_on, LV_PART_MAIN);
        lv_obj_remove_flag(pic, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_center(inner_label(pic, icons[t]));
        if (!s_ui.tiny) inner_label(b, names[t]);
        join_group(b);
        if (t != 0) continue;
        chats_pic = pic;
        lv_obj_t *badge = inner_label(b, "");
        lv_obj_add_style(badge, &s_styles.badge, LV_PART_MAIN);
        if (on) lv_obj_add_style(badge, &s_styles.badge_on, LV_PART_MAIN);
        lv_obj_add_flag(badge, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
        s_ui.tab_badge = badge;
    }
    if (s_ui.tiny) icon_button(bar, LV_SYMBOL_SETTINGS, settings_cb);
    lv_obj_update_layout(bar);
    if (s_ui.tab_badge && chats_pic) lv_obj_align_to(s_ui.tab_badge, chats_pic, LV_ALIGN_TOP_RIGHT, 9, -2);
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

static void draw_avatar(lv_layer_t *layer, int32_t x, int32_t y, int32_t size, const char *mono,
                        chatkit_avatar_t kind, bool online, lv_color_t ink, lv_color_t paper)
{
    lv_area_t a = {x, y, x + size - 1, y + size - 1};
    bool solid = kind == CHATKIT_AVATAR_SOLID;
    draw_box(layer, &a, LV_RADIUS_CIRCLE, solid, ink, solid ? 0 : 2, ink);
    const lv_font_t *font = (unsigned char)mono[0] == 0xEE ? &s_font : &s_font_18;
    int32_t lh = lv_font_get_line_height(font);
    draw_text(layer, mono, font, solid ? paper : ink, LV_TEXT_ALIGN_CENTER, x, y + (size - lh) / 2, x + size - 1,
              y + (size + lh) / 2);
    if (!online) return;
    lv_area_t dot = {x + size - 12, y + size - 12, x + size - 1, y + size - 1};
    draw_box(layer, &dot, LV_RADIUS_CIRCLE, true, ink, 2, paper);
}

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

/* A cell's own text, which only sets its height, is not drawn. */
static bool hide_cell_text(lv_draw_task_t *task)
{
    if (lv_draw_task_get_type(task) != LV_DRAW_TASK_TYPE_LABEL) return false;
    lv_draw_task_get_label_dsc(task)->opa = LV_OPA_TRANSP;
    return true;
}

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
    const chatkit_row_t *r = &s_ui.mem->rows[base->id1];
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
    draw_avatar(layer, a.x1 + 10, a.y1 + (h - AVATAR) / 2, AVATAR, r->mono, (chatkit_avatar_t)r->avatar, r->online,
                ink, paper);

    int32_t x = a.x1 + ROW_TEXT_X, right = a.x2 - 10;
    int32_t lh18 = lv_font_get_line_height(&s_font_18);
    int32_t top = a.y1 + (h - lh18 - lh) / 2;
    draw_text(layer, r->title, &s_font_18, ink, LV_TEXT_ALIGN_LEFT, x, top, right, top + lh18);

    int32_t line2 = top + lh18;
    if (r->toggle) {
        lv_area_t track = {right - 31, a.y1 + h / 2 - 9, right, a.y1 + h / 2 + 8};
        draw_box(layer, &track, LV_RADIUS_CIRCLE, r->on, ink, 2, ink);
        int32_t kx = r->on ? track.x2 - 14 : track.x1 + 3;
        lv_area_t knob = {kx, track.y1 + 3, kx + 11, track.y2 - 3};
        draw_box(layer, &knob, LV_RADIUS_CIRCLE, true, r->on ? paper : ink, 0, ink);
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

static int32_t row_text_width(void)
{
    return s_ui.width - ROW_TEXT_X - 10;
}

void chatkit_fill_row(chatkit_row_t *r, uint8_t kind, const char *key, const char *name, const char *right,
                      const char *detail)
{
    memset(r, 0, sizeof *r);
    r->kind = kind;
    chatkit_copy(r->key, sizeof r->key, key ? key : "");
    tailchat_initials(name ? name : "", r->mono, sizeof r->mono);
    snprintf(r->right, sizeof r->right, "%s", right ? right : "");
    int32_t right_w = r->right[0] ? text_width(r->right, &s_font) + 8 : 0;
    fit_line(r->title, sizeof r->title, name ? name : "", &s_font_18, row_text_width() - right_w);
    fit_line(r->detail, sizeof r->detail, detail ? detail : "", &s_font, row_text_width());
}

void chatkit_row_icon(chatkit_row_t *r, const char *icon)
{
    snprintf(r->mono, sizeof r->mono, "%s", icon);
}

void chatkit_row_toggle(chatkit_row_t *r, bool on, const char *detail)
{
    r->toggle = true;
    r->on = on;
    r->right[0] = '\0';
    /* Both lines beside the switch. */
    char title[sizeof r->title];
    memcpy(title, r->title, sizeof title);
    fit_line(r->title, sizeof r->title, title, &s_font_18, row_text_width() - 44);
    fit_line(r->detail, sizeof r->detail, detail, &s_font, row_text_width() - 44);
}

/* A detail line with room for a badge and a tick before it. */
static void refit_detail(chatkit_row_t *r, const char *detail)
{
    snprintf(r->badge_text, sizeof r->badge_text, r->badge > 99 ? "99+" : "%u", (unsigned)r->badge);
    int32_t w = row_text_width() - (r->tick ? 19 : 0);
    if (r->badge) w -= text_width("99", &s_font) + 18;
    fit_line(r->detail, sizeof r->detail, detail, &s_font, w);
}

static void describe(const chatkit_conv_t *c, char *mono, size_t mono_n, char *sub, size_t sub_n, bool *group)
{
    mono[0] = '\0';
    sub[0] = '\0';
    *group = false;
    if (s_ui.app->describe) s_ui.app->describe(c, mono, mono_n, sub, sub_n, group);
}

static int build_chats(chatkit_row_t *rows)
{
    struct tm now;
    bool h24;
    bool known = now_tm(&now, &h24);
    int n = chatkit_order(store(), s_ui.mem->order, CHATKIT_CONVS), count = 0;
    for (int i = 0; i < n && count < CHATKIT_ROWS; i++) {
        const chatkit_conv_t *c = s_ui.mem->order[i];
        char mono[12], sub[64], when[24] = "", detail[200];
        bool group;
        describe(c, mono, sizeof mono, sub, sizeof sub, &group);
        int shown = chatkit_shown(c);
        const chatkit_msg_t *m = shown ? chatkit_msg(c, shown - 1) : NULL;
        if (m && m->when && known) {
            struct tm then;
            s_tm(m->when, &then);
            tailchat_when(&now, &then, h24, when, sizeof when);
        }
        if (!m) {
            snprintf(detail, sizeof detail, "%s", sub);
        } else if (group && !m->mine && m->who[0]) {
            char one[120];
            tailchat_one_line(one, sizeof one, m->text, 80);
            snprintf(detail, sizeof detail, "%s: %s", m->who, one);
        } else {
            tailchat_one_line(detail, sizeof detail, m->text, 80);
        }
        chatkit_row_t *r = &rows[count++];
        chatkit_fill_row(r, CHATKIT_ROW_CHAT, c->conv, c->title[0] ? c->title : c->conv, when, "");
        if (mono[0]) chatkit_row_icon(r, mono);
        r->avatar = c->title[0] && strcmp(c->title, c->conv) != 0 ? CHATKIT_AVATAR_SOLID : CHATKIT_AVATAR_HOLLOW;
        r->tick = m && m->mine ? m->status : CHATKIT_NONE;
        r->badge = c->unread;
        refit_detail(r, detail);
    }
    return count;
}

/* Settings the two chat apps share: they are the kernel's, for both. */
static int kit_settings(chatkit_row_t *rows, int max)
{
    int n = 0;
    if (n < max) {
        bool on = rs_msg_log_enabled();
        chatkit_row_t *r = &rows[n++];
        /* SMS and mesh messages alike, unencrypted (thistle/msg_log.h). */
        chatkit_fill_row(r, CHATKIT_ROW_SAVE, NULL, "Keep history", NULL, NULL);
        chatkit_row_icon(r, LV_SYMBOL_SD_CARD);
        r->avatar = CHATKIT_AVATAR_HOLLOW;
        chatkit_row_toggle(r, on, !on ? "Lost at a restart" : rs_msg_log_writable() ? "On SD, unencrypted" : "No SD card");
    }
    if (n < max) {
        bool on = rs_msg_alert_enabled();
        chatkit_row_t *r = &rows[n++];
        chatkit_fill_row(r, CHATKIT_ROW_ALERTS, NULL, "Alerts", NULL, NULL);
        chatkit_row_icon(r, LV_SYMBOL_BELL);
        r->avatar = CHATKIT_AVATAR_HOLLOW;
        /* SMS and mesh messages alike (thistle/msg_alert.h). */
        chatkit_row_toggle(r, on, on ? "Buzz and pop up" : "Counted only");
    }
    return n;
}

static int build_rows(chatkit_row_t *rows)
{
    switch (s_ui.page) {
    case PAGE_CHATS:
        return build_chats(rows);
    case PAGE_CONTACTS:
        return s_ui.app->contacts ? s_ui.app->contacts(rows, CHATKIT_ROWS, s_ui.query) : 0;
    case PAGE_SETTINGS: {
        int n = s_ui.app->settings ? s_ui.app->settings(rows, CHATKIT_ROWS) : 0;
        return n + kit_settings(rows + n, CHATKIT_ROWS - n);
    }
    default:
        return 0;
    }
}

static void show_empty(void);

static void refresh_list(bool first)
{
    if (!s_ui.table) return;
    mem_t *mem = s_ui.mem;
    memset(mem->next_rows, 0, sizeof mem->next_rows);
    int n = build_rows(mem->next_rows);
    if (n > CHATKIT_ROWS) n = CHATKIT_ROWS;
    if (!first && n == s_ui.rows && memcmp(mem->rows, mem->next_rows, sizeof mem->rows[0] * (size_t)n) == 0) return;
    memcpy(mem->rows, mem->next_rows, sizeof mem->rows);
    s_ui.rows = n;
    lv_table_set_row_count(s_ui.table, (uint32_t)n);
    for (int i = 0; i < n; i++) lv_table_set_cell_value(s_ui.table, (uint32_t)i, 0, "-\n-");
    set_shown(s_ui.table, n > 0);
    lv_obj_invalidate(s_ui.table);
    if (is_tab(s_ui.page)) {
        ESP_LOGI(TAG, "%s: page %s: %d rows", s_ui.app->tag, s_ui.page == PAGE_CHATS ? "chats" : "contacts", n);
    }
    show_empty();
}

void chatkit_refresh(void)
{
    if (s_ui.root && s_ui.table && s_ui.page != PAGE_CONVERSATION) refresh_list(false);
}

/* ------------------------------------------------------------------ */
/* The tabs' pages                                                      */
/* ------------------------------------------------------------------ */

static void to_contacts_cb(lv_event_t *e)
{
    (void)e;
    go(PAGE_CONTACTS, NULL);
}

static void show_empty(void)
{
    bool none = s_ui.rows == 0 && s_ui.page == PAGE_CHATS;
    if (!none) {
        if (s_ui.empty) {
            lv_obj_delete(s_ui.empty);
            s_ui.empty = NULL;
        }
        return;
    }
    if (s_ui.empty) return;
    s_ui.empty = plain(s_ui.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.empty, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(s_ui.empty, s_ui.compact ? 8 : 28, 0);
    lv_obj_set_style_pad_row(s_ui.empty, 6, 0);
    lv_obj_set_style_pad_hor(s_ui.empty, 16, 0);
    lv_obj_t *pic = lv_label_create(s_ui.empty);
    lv_obj_set_style_text_font(pic, &s_buttons, 0);
    lv_label_set_text(pic, s_ui.app->chats_icon);
    lv_obj_t *t = label(s_ui.empty, s_ui.app->empty_title, &s_styles.center);
    lv_obj_set_style_text_font(t, &s_font_18, 0);
    label(s_ui.empty, s_ui.app->empty_text, &s_styles.center);
    char text[48];
    snprintf(text, sizeof text, "%s  %s", s_ui.app->contacts_icon, s_ui.app->contacts_tab);
    lv_obj_t *b = button(s_ui.empty, text, to_contacts_cb, true);
    lv_obj_set_style_margin_top(b, 6, 0);
}

static void search_cb(lv_event_t *e)
{
    (void)e;
    chatkit_copy(s_ui.query, sizeof s_ui.query, lv_textarea_get_text(s_ui.field));
    refresh_list(false);
}

/* Enter in the search field chooses the first row: a number typed whole
 * is the first, as "Text +49...". */
static void search_ready_cb(lv_event_t *e)
{
    (void)e;
    if (s_ui.rows > 0) choose_row(0);
}

static void tab_page(const char *title)
{
    if (s_ui.tiny) set_shown(s_ui.header, false);
    else tab_header(title);
    if (s_ui.page == PAGE_CONTACTS && s_ui.app->search_hint) {
        lv_obj_t *row = plain(s_ui.body, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_hor(row, EDGE, 0);
        lv_obj_set_style_pad_top(row, 6, 0);
        s_ui.field = lv_textarea_create(row);
        lv_obj_add_style(s_ui.field, &s_styles.field, LV_PART_MAIN);
        lv_textarea_set_one_line(s_ui.field, true);
        lv_textarea_set_max_length(s_ui.field, sizeof s_ui.query - 1);
        lv_textarea_set_placeholder_text(s_ui.field, s_ui.app->search_hint);
        lv_textarea_set_text(s_ui.field, s_ui.query);
        lv_obj_set_flex_grow(s_ui.field, 1);
        lv_obj_add_event_cb(s_ui.field, search_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(s_ui.field, search_ready_cb, LV_EVENT_READY, NULL);
        join_group(s_ui.field);
    }
    s_ui.table = make_table(draw_list_row, list_cb, ROW_TEXT_X, 10, ROW_PAD);
    tab_bar();
    refresh_list(true);
    if (s_ui.field) focus(s_ui.field);
    else if (s_ui.rows > 0) focus(s_ui.table);
}

/* ------------------------------------------------------------------ */
/* Settings                                                             */
/* ------------------------------------------------------------------ */

static void show_settings(void)
{
    page_header("Settings");
    if (s_ui.app->own_card) {
        char title[64] = "", *text = s_ui.mem->line;
        text[0] = '\0';
        s_ui.app->own_card(title, sizeof title, text, sizeof s_ui.mem->line);
        lv_obj_t *card = plain(s_ui.body, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_style(card, &s_styles.card, LV_PART_MAIN);
        lv_obj_set_width(card, s_ui.width - 2 * EDGE);
        lv_obj_set_style_margin_top(card, 8, 0);
        lv_obj_t *t = line_label(card, title, &s_styles.title);
        lv_obj_set_style_text_font(t, &s_font_18, 0);
        keep_one_line(t);
        label(card, text, NULL);
    }
    s_ui.table = make_table(draw_list_row, list_cb, ROW_TEXT_X, 10, ROW_PAD);
    refresh_list(true);
    focus(s_ui.table);
}

static void choose_row(int row)
{
    const chatkit_row_t *r = &s_ui.mem->rows[row];
    switch (r->kind) {
    case CHATKIT_ROW_CHAT:
        go(PAGE_CONVERSATION, r->key);
        return;
    case CHATKIT_ROW_SAVE: {
        bool on = !rs_msg_log_enabled();
        if (rs_msg_log_set_enabled(on) != ESP_OK) toast_warn("Could not save the choice; it lasts until a restart");
        ESP_LOGI(TAG, "save messages %s", on ? "on" : "off");
        refresh_list(false);
        return;
    }
    case CHATKIT_ROW_ALERTS: {
        bool on = !rs_msg_alert_enabled();
        if (rs_msg_alert_set_enabled(on) != ESP_OK) toast_warn("Could not save the choice; it lasts until a restart");
        ESP_LOGI(TAG, "alerts %s", on ? "on" : "off");
        refresh_list(false);
        return;
    }
    default:
        break;
    }
    if (s_ui.page == PAGE_CONTACTS && s_ui.app->contact_chosen) {
        s_ui.app->contact_chosen(r);
    } else if (s_ui.page == PAGE_SETTINGS && s_ui.app->setting_chosen) {
        s_ui.app->setting_chosen(r);
        refresh_list(false);
    }
}

/* ------------------------------------------------------------------ */
/* A conversation                                                       */
/* ------------------------------------------------------------------ */

static int32_t bubble_max(void)
{
    return s_ui.width - 2 * EDGE - BUBBLE_AWAY - 2 * BUBBLE_PX;
}

static int32_t bubble_side(void)
{
    return (s_ui.width - bubble_max()) / 2;
}

static void draw_bubble(lv_event_t *e)
{
    lv_draw_task_t *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(task);
    if (base->part != LV_PART_ITEMS || base->id1 >= (uint32_t)s_ui.rows) return;
    if (hide_cell_text(task) || lv_draw_task_get_type(task) != LV_DRAW_TASK_TYPE_FILL) return;

    const theme_colors_t *clr = theme_get_colors();
    const bubble_t *b = &s_ui.mem->bubbles[base->id1];
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

    int32_t inner_h = (b->who[0] ? lh : 0) + b->text_h + (b->meta_line ? lh : 0);
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
    if (key_row() == (int)base->id1) {
        lv_area_t ring = {box.x1 - 3, box.y1 - 3, box.x2 + 3, box.y2 + 3};
        draw_box(layer, &ring, BUBBLE_R + 3, false, clr->bg, 2, clr->text);
    }

    int32_t x = box.x1 + BUBBLE_PX, y = box.y1 + BUBBLE_PY;
    int32_t right = box.x2 - BUBBLE_PX;
    if (b->who[0]) {
        draw_text(layer, b->who, &s_font, ink, LV_TEXT_ALIGN_LEFT, x, y, right, y + lh);
        lv_area_t under = {x, y + lh - 2, x + text_width(b->who, &s_font), y + lh - 2};
        draw_box(layer, &under, 0, true, ink, 0, ink);
        y += lh;
    }
    draw_text(layer, b->text, &s_font, ink, LV_TEXT_ALIGN_LEFT, x, y, x + b->wrap_w - 1, y + b->text_h);
    int32_t my = b->meta_line ? y + b->text_h : y + b->text_h - lh;
    draw_text(layer, b->meta, &s_font, ink, LV_TEXT_ALIGN_RIGHT, x, my, right, my + lh);
}

static bool has_arabic(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0xD8 && c <= 0xDB) return true;
    }
    return false;
}

static void lay_out_bubble(bubble_t *b, const char *text)
{
    int32_t max = bubble_max();
    b->cut = tailchat_excerpt(b->text, sizeof b->text, text, TEXT_MAX, TEXT_LINES);
    if (!b->text[0]) snprintf(b->text, sizeof b->text, " ");
    int32_t meta_w = text_width(b->meta, &s_font);
    int32_t who_w = b->who[0] ? text_width(b->who, &s_font) : 0;
    lv_point_t full, narrow;
    lv_text_get_size(&full, b->text, &s_font, 0, 0, max, LV_TEXT_FLAG_NONE);
    b->meta_line = false;
    b->wrap_w = (int16_t)max;
    b->inner_w = (int16_t)full.x;
    if (b->meta[0]) {
        lv_text_get_size(&narrow, b->text, &s_font, 0, 0, max - meta_w - META_GAP, LV_TEXT_FLAG_NONE);
        if (narrow.y == full.y && !has_arabic(b->text)) {
            b->wrap_w = (int16_t)(max - meta_w - META_GAP);
            b->inner_w = (int16_t)(narrow.x + META_GAP + meta_w);
        } else {
            b->meta_line = true;
            if (meta_w > b->inner_w) b->inner_w = (int16_t)meta_w;
        }
    }
    if (who_w > b->inner_w) b->inner_w = (int16_t)(who_w > max ? max : who_w);
    b->text_h = (int16_t)full.y;
}

static const char *bubble_cell_text(const bubble_t *b, char *out, size_t n)
{
    size_t at = 0;
#define ADD(s) at += (size_t)snprintf(out + at, at < n ? n - at : 0, "%s", (s))
    if (b->kind != BUBBLE_MESSAGE) {
        ADD("-");
    } else {
        if (b->who[0]) ADD("-\n");
        ADD(b->text);
        if (b->meta_line) ADD("\n-");
    }
#undef ADD
    if (at >= n) out[n - 1] = '\0';
    return out;
}

static int build_conversation(bubble_t *rows, const chatkit_conv_t *c)
{
    struct tm now, then, before = {0};
    bool h24;
    bool known = now_tm(&now, &h24);
    char mono[12], sub[64];
    bool group;
    describe(c, mono, sizeof mono, sub, sizeof sub, &group);
    int n = chatkit_shown(c);
    uint32_t first = c->count - (uint32_t)n;

    /* The unread line goes above the first of the last `unread` received. */
    int unread_from = n;
    for (int i = n - 1, left = s_ui.unread_at_open; i >= 0 && left > 0; i--) {
        if (!chatkit_msg(c, i)->mine) {
            unread_from = i;
            left--;
        }
    }

    int count = 0;
    bool before_dated = false;
    const chatkit_msg_t *prev = NULL;
    for (int i = 0; i < n; i++) {
        const chatkit_msg_t *m = chatkit_msg(c, i);
        bool dated = known && m->when;
        if (dated) s_tm(m->when, &then);
        if (dated && (!before_dated || !tailchat_same_day(&before, &then))) {
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
        /* The one before is the last of its run unless this goes on with it:
         * the same writer, within three minutes. */
        bool joins = prev && count > 0 && rows[count - 1].kind == BUBBLE_MESSAGE &&
                     tailchat_joins(prev->mine, prev->when * 1000, m->mine, m->when * 1000) &&
                     (m->mine || strcmp(prev->who, m->who) == 0);
        if (joins) {
            rows[count - 1].tail = false;
            if (rows[count - 1].tick != CHATKIT_PENDING && rows[count - 1].tick != CHATKIT_FAILED) {
                rows[count - 1].meta[0] = '\0';
            }
        }
        bubble_t *b = &rows[count++];
        b->kind = BUBBLE_MESSAGE;
        b->mine = m->mine;
        b->tail = true;
        b->tick = m->mine ? m->status : CHATKIT_NONE;
        b->number = first + (uint32_t)i;
        char clock[16] = "";
        if (dated) tailchat_clock(&then, h24, clock, sizeof clock);
        const char *tick = tick_icon(b->tick);
        snprintf(b->meta, sizeof b->meta, "%s%s%s", clock, clock[0] && tick[0] ? " " : "", tick);
        if (group && !m->mine && m->who[0] && !joins) {
            fit_line(b->who, sizeof b->who, m->who, &s_font, bubble_max());
        }
        prev = m;
        if (dated) {
            before = then;
            before_dated = true;
        }
        lay_out_bubble(b, m->text);
    }
    /* A bubble that lost its time to the next of its run is laid out again
     * without it. */
    for (int i = 0; i < count; i++) {
        bubble_t *b = &rows[i];
        if (b->kind == BUBBLE_MESSAGE && !b->tail && !b->meta[0]) {
            const chatkit_msg_t *m = chatkit_msg(c, (int)(b->number - first));
            bool cut = b->cut;
            if (m) lay_out_bubble(b, m->text);
            b->cut = cut;
        }
    }
    if (count == 0) {
        rows[count].kind = BUBBLE_EMPTY;
        snprintf(rows[count++].text, sizeof rows[0].text, "Say hello");
    }
    return count;
}

static void apply_bubbles(int n)
{
    s_ui.rows = n;
    lv_table_set_row_count(s_ui.table, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        lv_table_set_cell_value(s_ui.table, (uint32_t)i, 0,
                                bubble_cell_text(&s_ui.mem->bubbles[i], s_ui.mem->cell, sizeof s_ui.mem->cell));
    }
    lv_obj_invalidate(s_ui.table);
}

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
        for (int i = 0; i < s_ui.rows; i++) {
            if (s_ui.mem->bubbles[i].kind != BUBBLE_UNREAD) continue;
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

static void fill_conversation(bool opening)
{
    chatkit_conv_t *c = conv_on_page();
    if (!c) {
        go_back();
        return;
    }
    char mono[12], sub[64];
    bool group;
    describe(c, mono, sizeof mono, sub, sizeof sub, &group);
    set_text(s_ui.title, c->title[0] ? c->title : c->conv);
    set_text(s_ui.subtitle, sub);

    mem_t *mem = s_ui.mem;
    memset(mem->next_bubbles, 0, sizeof mem->next_bubbles);
    int n = build_conversation(mem->next_bubbles, c);
    bool at_bottom = lv_obj_get_scroll_bottom(s_ui.body) <= 24;
    if (opening || n != s_ui.rows || memcmp(mem->bubbles, mem->next_bubbles, sizeof mem->bubbles[0] * (size_t)n) != 0) {
        memcpy(mem->bubbles, mem->next_bubbles, sizeof mem->bubbles);
        apply_bubbles(n);
        if (opening || at_bottom) scroll_conversation(opening);
    }

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "%s: conversation %s: %d messages; LVGL pool %u KB free, largest block %u KB", s_ui.app->tag,
             c->title[0] ? c->title : c->conv, chatkit_shown(c), (unsigned)(mon.free_size / 1024),
             (unsigned)(mon.free_biggest_size / 1024));
}

static void send_text(void)
{
    const char *text = lv_textarea_get_text(s_ui.field);
    if (!text[0]) return;
    const char *why = s_ui.app->why_not ? s_ui.app->why_not() : NULL;
    if (why) {
        toast_warn(why);
        return;
    }
    uint32_t ref = 0;
    chatkit_status_t status = CHATKIT_SENT;
    esp_err_t err = s_ui.app->send(s_ui.key, text, &ref, &status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: could not send: %s", s_ui.app->tag, esp_err_to_name(err));
        toast_warn(err == ESP_ERR_INVALID_SIZE ? "Too long to send" : "Could not send");
        return;
    }
    chatkit_conv_t *c = chatkit_ensure(store(), s_ui.key, NULL, false);
    chatkit_add(store(), c, true, NULL, text, now_s(), ref, status);
    ESP_LOGI(TAG, "%s: sent%s", s_ui.app->tag, ref ? ", waiting for word" : "");
    lv_textarea_set_text(s_ui.field, "");
    fill_conversation(false);
    s_ui.generation = store()->generation;
}

static void send_cb(lv_event_t *e)
{
    (void)e;
    send_text();
}

static void call_cb(lv_event_t *e)
{
    (void)e;
    if (!s_ui.app->call) return;
    esp_err_t err = s_ui.app->call(s_ui.key);
    if (err == ESP_OK) ESP_LOGI(TAG, "%s: calling", s_ui.app->tag);
    else toast_warn(err == ESP_ERR_INVALID_STATE ? "A call is already up, or the modem is off" : "Could not call");
}

/* A bubble tapped, or Enter on it: one cut short opens whole. */
static void bubbles_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    bool keys = indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD;
    if (keys && lv_indev_get_key(indev) != LV_KEY_ENTER) {
        lv_obj_invalidate(s_ui.table);
        return;
    }
    uint32_t row = LV_TABLE_CELL_NONE, col = LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(s_ui.table, &row, &col);
    if (row >= (uint32_t)s_ui.rows) return;
    const bubble_t *b = &s_ui.mem->bubbles[row];
    if (b->kind != BUBBLE_MESSAGE || !b->cut) return;
    s_ui.read_number = b->number;
    go(PAGE_MESSAGE, s_ui.key);
}

static void show_conversation(void)
{
    chatkit_conv_t *c = conv_on_page();
    if (!c) {
        go_back();
        return;
    }
    s_ui.unread_at_open = c->unread;
    s_ui.in_conv = true;
    chatkit_set_viewing(store(), c->conv);
    rs_msg_alert_set_viewing(s_ui.app->alert_transport, c->conv);
    report_unread();

    char mono[12], sub[64];
    bool group;
    describe(c, mono, sizeof mono, sub, sizeof sub, &group);
    const char *name = c->title[0] ? c->title : c->conv;

    lv_obj_t *h = header_row();
    icon_button(h, LV_SYMBOL_LEFT, back_button_cb);
    lv_obj_t *who = plain(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(who, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(who, 8, 0);
    lv_obj_set_flex_grow(who, 1);
    lv_obj_set_width(who, 0);
    if (!mono[0]) tailchat_initials(name, mono, sizeof mono);
    lv_obj_t *av = lv_label_create(who);
    lv_obj_add_style(av, &s_styles.avatar, LV_PART_MAIN);
    lv_obj_add_style(av, &s_styles.center, LV_PART_MAIN);
    lv_obj_set_size(av, 30, 30);
    lv_obj_set_style_pad_top(av, 6, 0);
    lv_label_set_text(av, mono);
    if (s_ui.tiny) lv_obj_add_flag(av, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *col = plain(who, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_width(col, 0);
    s_ui.title = line_label(col, name, &s_styles.title);
    lv_obj_set_style_text_font(s_ui.title, &s_font_18, 0);
    keep_one_line(s_ui.title);
    s_ui.subtitle = line_label(col, sub, &s_styles.subtitle);
    if (s_ui.tiny) lv_obj_add_flag(s_ui.subtitle, LV_OBJ_FLAG_HIDDEN);
    if (s_ui.app->call) icon_button(h, LV_SYMBOL_CALL, call_cb);

    s_ui.table = make_table(draw_bubble, bubbles_cb, bubble_side(), bubble_side(), BUBBLE_GAP + BUBBLE_PY);

    lv_obj_t *composer = plain(s_ui.footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(composer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(composer, 6, 0);
    s_ui.field = lv_textarea_create(composer);
    lv_obj_add_style(s_ui.field, &s_styles.field, LV_PART_MAIN);
    lv_textarea_set_one_line(s_ui.field, true);
    uint32_t limit = s_ui.app->text_limit ? s_ui.app->text_limit(c->conv) : CHATKIT_TEXT - 1;
    if (limit >= CHATKIT_TEXT) limit = CHATKIT_TEXT - 1;
    lv_textarea_set_max_length(s_ui.field, limit);
    lv_textarea_set_placeholder_text(s_ui.field, "Message");
    lv_obj_set_flex_grow(s_ui.field, 1);
    lv_obj_add_event_cb(s_ui.field, send_cb, LV_EVENT_READY, NULL);
    join_group(s_ui.field);
    lv_obj_t *send = icon_button(composer, TC_ICON_SEND, send_cb);
    lv_obj_set_style_border_width(send, 1, 0);

    fill_conversation(true);
    focus(s_ui.field);
}

/* The whole of a message too long for its bubble. */
static void show_message(void)
{
    chatkit_conv_t *c = conv_on_page();
    int shown = c ? chatkit_shown(c) : 0;
    uint32_t first = c ? c->count - (uint32_t)shown : 0;
    if (!c || s_ui.read_number < first || s_ui.read_number >= c->count) {
        go_back();
        return;
    }
    const chatkit_msg_t *m = chatkit_msg(c, (int)(s_ui.read_number - first));
    chatkit_copy(s_ui.mem->whole, sizeof s_ui.mem->whole, m->text);
    page_header(m->mine ? "You" : m->who[0] ? m->who : c->title);
    lv_obj_t *card = plain(s_ui.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(card, &s_styles.card, LV_PART_MAIN);
    lv_obj_set_width(card, s_ui.width - 2 * EDGE);
    lv_obj_set_style_margin_top(card, 8, 0);
    lv_obj_t *l = label(card, "", NULL);
    lv_label_set_text_static(l, s_ui.mem->whole);
}

/* ------------------------------------------------------------------ */
/* What the app hands over                                              */
/* ------------------------------------------------------------------ */

void chatkit_receive(const char *conv, const char *title, const char *who, const char *text, uint64_t when)
{
    chatkit_store_t *st = store();
    if (!st) return;
    chatkit_conv_t *c = chatkit_ensure(st, conv, title, false);
    if (!c) {
        ESP_LOGW(TAG, "no room for a conversation: a message is dropped");
        return;
    }
    chatkit_add(st, c, false, who, text, when ? when : now_s(), 0, CHATKIT_NONE);
}

void chatkit_keep(const char *conv, const char *title, bool mine, const char *who, const char *text, uint64_t when)
{
    chatkit_store_t *st = store();
    if (!st) return;
    chatkit_conv_t *c = chatkit_ensure(st, conv, title, false);
    if (!c) return;
    chatkit_add(st, c, mine, who, text, when, 0, CHATKIT_NONE);
    chatkit_mark_read(st, c);
}

void chatkit_sent_status(uint32_t ref, chatkit_status_t status)
{
    if (store()) chatkit_set_status(store(), ref, status);
}

/* ------------------------------------------------------------------ */
/* Drawing the page                                                     */
/* ------------------------------------------------------------------ */

static void leave_conversation(void)
{
    s_ui.in_conv = false;
    chatkit_set_viewing(store(), NULL);
    rs_msg_alert_set_viewing(-1, NULL);
}

static void show_page(void)
{
    s_ui.generation = store()->generation;
    if (s_ui.in_conv) leave_conversation();
    lv_obj_clean(s_ui.header);
    set_shown(s_ui.header, true);
    lv_obj_clean(s_ui.body);
    lv_obj_clean(s_ui.footer);
    lv_obj_scroll_to_y(s_ui.body, 0, LV_ANIM_OFF);
    s_ui.empty = NULL;
    s_ui.table = NULL;
    s_ui.field = NULL;
    s_ui.title = NULL;
    s_ui.subtitle = NULL;
    s_ui.tab_badge = NULL;
    s_ui.rows = 0;
    switch (s_ui.page) {
    case PAGE_CHATS:        tab_page(s_ui.app->name); break;
    case PAGE_CONTACTS:     tab_page(s_ui.app->contacts_tab); break;
    case PAGE_CONVERSATION: show_conversation(); break;
    case PAGE_SETTINGS:     show_settings(); break;
    case PAGE_MESSAGE:      show_message(); break;
    }
    set_shown(s_ui.footer, lv_obj_get_child_count(s_ui.footer) > 0);
    show_status();
}

/* Twice a second: hand over what came, and draw again only if anything
 * shown changed. */
static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_ui.app->poll) s_ui.app->poll();
    report_unread();
    show_status();
    uint32_t seq = s_ui.app->contacts_seq ? s_ui.app->contacts_seq() : 0;
    bool contacts = seq != s_ui.contacts_seq;
    s_ui.contacts_seq = seq;
    if (store()->generation == s_ui.generation && !(contacts && s_ui.page == PAGE_CONTACTS)) return;
    s_ui.generation = store()->generation;
    switch (s_ui.page) {
    case PAGE_CHATS:
    case PAGE_CONTACTS:
        refresh_list(false);
        break;
    case PAGE_CONVERSATION:
        if (s_ui.table) fill_conversation(false);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Making and unmaking                                                  */
/* ------------------------------------------------------------------ */

static void *psram_calloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (p) memset(p, 0, size);
    return p ? p : calloc(1, size);
}

esp_err_t chatkit_ui_create(lv_obj_t *parent, const chatkit_app_t *app)
{
    if (!parent) parent = lv_screen_active();
    memset(&s_ui, 0, sizeof s_ui);
    s_ui.app = app;
    s_ui.reported_unread = UINT32_MAX;
    if (!app->store->alloc) {
        app->store->alloc = psram_calloc;
        app->store->release = free;
    }
    s_ui.mem = heap_caps_malloc(sizeof *s_ui.mem, MALLOC_CAP_SPIRAM);
    if (!s_ui.mem) s_ui.mem = malloc(sizeof *s_ui.mem);
    if (!s_ui.mem) return ESP_ERR_NO_MEM;
    memset(s_ui.mem, 0, sizeof *s_ui.mem);

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

    /* The card's history once a boot, then what waited in the kernel. */
    if (!app->store->history_read) {
        app->store->history_read = true;
        if (app->read_history) app->read_history();
    }
    if (app->poll) app->poll();
    report_unread();

    ui_manager_set_back_handler(s_ui.root, back_cb);
    s_ui.page = PAGE_CHATS;
    s_ui.contacts_seq = app->contacts_seq ? app->contacts_seq() : 0;
    show_page();
    s_ui.timer = lv_timer_create(timer_cb, 500, NULL);

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "%s: LVGL pool %u KB free, largest block %u KB", app->tag, (unsigned)(mon.free_size / 1024),
             (unsigned)(mon.free_biggest_size / 1024));
    return ESP_OK;
}

void chatkit_ui_destroy(void)
{
    if (s_ui.in_conv) {
        leave_conversation();
        request_full_refresh();
    }
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    lv_async_call_cancel(go_async, NULL);
    if (s_ui.root) lv_obj_delete(s_ui.root);
    free(s_ui.mem);
    memset(&s_ui, 0, sizeof s_ui);
    styles_reset();
}
