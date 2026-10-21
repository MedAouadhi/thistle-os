/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Phone app's lists: Favourites, Recents and Contacts
 *
 * A list is one lv_table, as in the Reader: a row costs a string in LVGL's
 * pool, where a row of widgets cost hundreds of bytes. At most
 * PHONE_LIST_MAX rows are filled, the last saying how many more matched.
 * A list is filled again only when what it shows has changed: the
 * phonebook's or the call log's sequence number, or the search. On e-paper
 * every change is a refresh.
 *
 * Contacts keeps the keyboard in its search field, so typing filters at
 * once; Up and Down move a highlight through the list from there, and Enter
 * opens the highlighted contact (or the first). Favourites and Recents give
 * the keyboard to their table, as the Reader's lists do. A tap opens a row.
 */
#include "phone/phone_text.h"
#include "phone/phone_ui.h"

#include "thistle/call.h"
#include "thistle/phonebook.h"
#include "thistle/time_manager.h"
#include "ui/theme.h"
#include "ui/toast.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "phone";

#define QUERY_MAX   48
#define CELL_MAX    192
#define MARK_PAD    4      /* Recents' cells: narrower, for three columns */

typedef enum {
    ROW_NOTE,      /* read, not chosen */
    ROW_CONTACT,   /* index: the phonebook's */
    ROW_CALL,      /* index: the call log's, of the newest call the row holds */
} row_kind_t;

typedef struct {
    uint8_t  kind;
    uint8_t  unanswered;   /* a call row: missed or declined */
    uint8_t  count;        /* a call row: how many calls in a row it stands for */
    uint16_t index;
} row_t;

/* The list shown. In PSRAM: only the LVGL task touches it. */
static EXT_RAM_BSS_ATTR struct {
    phone_tab_t tab;
    lv_obj_t   *table;
    lv_obj_t   *search;     /* Contacts */
    lv_obj_t   *empty;      /* what an empty list says, in place of the table */
    lv_obj_t   *empty_note;
    lv_obj_t   *rescan;     /* Contacts: search the card again */
    lv_obj_t   *clear;      /* Recents: its header's Clear */
    lv_obj_t   *confirm;    /* Recents: "Clear all calls?" over the header */
    row_t       rows[PHONE_LIST_MAX + 1];
    uint32_t    row_count;
    bool        filling;    /* the selection is being set, not chosen */
    bool        keyed;      /* a row is chosen by the keys, and shown so */
    uint32_t    book_seq;
    bool        loading;
    uint32_t    log_seq;
    int         day;        /* Recents: the local day its times are written for */
} s_list;

/* Kept while the app is open, for coming back to a list from a page. */
static struct {
    char    query[QUERY_MAX];
    int32_t scroll[PHONE_TAB_COUNT];
    int32_t selected[PHONE_TAB_COUNT];   /* the row the keys had, or -1 */
} s_keep;

/* Off the LVGL task's stack, which is 8 KB. */
static EXT_RAM_BSS_ATTR uint32_t s_indices[PHONE_LIST_MAX];
static EXT_RAM_BSS_ATTR pb_contact_t s_contact;
static EXT_RAM_BSS_ATTR char s_cell[CELL_MAX];
static EXT_RAM_BSS_ATTR char s_whens[CALL_LOG_KEEP][16];
static EXT_RAM_BSS_ATTR char s_name[PB_NAME_MAX + 8];

static void open_row(uint32_t row);

/* ------------------------------------------------------------------ */
/* Rows                                                                 */
/* ------------------------------------------------------------------ */

static bool selectable(uint32_t row)
{
    return row < s_list.row_count && s_list.rows[row].kind != ROW_NOTE;
}

static void rows_begin(void)
{
    s_list.row_count = 0;
}

static void add_row(row_kind_t kind, uint32_t index, uint32_t col, const char *text)
{
    if (col == 0) {
        if (s_list.row_count > PHONE_LIST_MAX) return;
        row_t *r = &s_list.rows[s_list.row_count++];
        memset(r, 0, sizeof *r);
        r->kind = (uint8_t)kind;
        r->index = (uint16_t)index;
    }
    lv_table_set_cell_value(s_list.table, s_list.row_count - 1, col, text);
}

static void rows_end(void)
{
    lv_table_set_row_count(s_list.table, s_list.row_count ? s_list.row_count : 1);
    if (!s_list.row_count) lv_table_set_cell_value(s_list.table, 0, 0, "");
}

/* Selects `row` without opening it. */
static void select_row(uint32_t row)
{
    if (!s_list.table || !s_list.row_count) return;
    s_list.filling = true;
    lv_table_set_selected_cell(s_list.table, (uint16_t)row, 0);
    s_list.filling = false;
}

static int32_t first_selectable(void)
{
    for (uint32_t r = 0; r < s_list.row_count; r++) {
        if (selectable(r)) return (int32_t)r;
    }
    return -1;
}

/* No row chosen by the keys. Contacts' search field has the keyboard back. */
static void unkey(void)
{
    s_list.keyed = false;
    if (!s_list.table) return;
    if (s_list.tab == PHONE_TAB_CONTACTS) lv_obj_remove_state(s_list.table, LV_STATE_FOCUSED);
    lv_obj_invalidate(s_list.table);
}

/* Contacts: moves the highlight `step` rows, the table being drawn as if
 * it had the keyboard while the search field keeps it. Up from the first
 * row goes back to the field. */
static void move_highlight(int step)
{
    if (!s_list.table) return;
    int32_t first = first_selectable();
    if (first < 0) return;
    int32_t row = first;
    if (s_list.keyed) {
        uint32_t r = 0, c = 0;
        lv_table_get_selected_cell(s_list.table, &r, &c);
        row = r == LV_TABLE_CELL_NONE ? first : (int32_t)r + step;
        while (row >= 0 && row < (int32_t)s_list.row_count && !selectable((uint32_t)row)) row += step;
        if (row < first) {
            unkey();
            lv_obj_scroll_to_y(s_list.table, 0, LV_ANIM_OFF);
            return;
        }
        if (row >= (int32_t)s_list.row_count) return;  /* the last stays */
    } else if (step < 0) {
        return;
    }
    select_row((uint32_t)row);
    s_list.keyed = true;
    lv_obj_add_state(s_list.table, LV_STATE_FOCUSED);
    /* At the last contact, the note after it ("12 more") is shown too. */
    bool last = true;
    for (uint32_t r = (uint32_t)row + 1; r < s_list.row_count; r++) last = last && !selectable(r);
    if (last && (uint32_t)row + 1 < s_list.row_count) {
        lv_obj_update_layout(s_list.table);
        lv_obj_scroll_by(s_list.table, 0, -lv_obj_get_scroll_bottom(s_list.table), LV_ANIM_OFF);
    }
}

/* Where the list is, kept for coming back to it. */
static void keep_place(void)
{
    if (!s_list.table) return;
    phone_tab_t tab = s_list.tab;
    s_keep.scroll[tab] = lv_obj_get_scroll_y(s_list.table);
    s_keep.selected[tab] = -1;
    bool chosen = tab == PHONE_TAB_CONTACTS ? s_list.keyed : phone_ui_from_keys();
    if (chosen) {
        uint32_t r = 0, c = 0;
        lv_table_get_selected_cell(s_list.table, &r, &c);
        if (r != LV_TABLE_CELL_NONE) s_keep.selected[tab] = (int32_t)r;
    }
    if (tab == PHONE_TAB_CONTACTS && s_list.search) {
        snprintf(s_keep.query, sizeof s_keep.query, "%s", lv_textarea_get_text(s_list.search));
    }
}

/* Back where it was, once filled; once, as a tab chosen again starts at
 * the top. */
static void restore_place(void)
{
    phone_tab_t tab = s_list.tab;
    if (s_keep.scroll[tab] > 0) lv_obj_scroll_to_y(s_list.table, s_keep.scroll[tab], LV_ANIM_OFF);
    int32_t row = s_keep.selected[tab];
    s_keep.scroll[tab] = 0;
    s_keep.selected[tab] = -1;
    if (row >= 0 && selectable((uint32_t)row)) {
        select_row((uint32_t)row);
        s_list.keyed = true;
        if (tab == PHONE_TAB_CONTACTS) lv_obj_add_state(s_list.table, LV_STATE_FOCUSED);
    }
}

/* Each row is drawn here rather than by the table's styles, which mark a
 * single cell: the one selected, when the table has the keyboard. Here the
 * whole row is chosen, and only while the keys have chosen it or a finger is
 * on it, so that a list just opened shows no row picked (an lv_table always
 * has a selected cell). Notes are plainer than the rest; in Recents the mark
 * of a call not answered is inverted, and the times are set to the right. */
static void list_draw_cb(lv_event_t *e)
{
    lv_draw_task_t *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(task);
    if (base->part != LV_PART_ITEMS || base->id1 >= s_list.row_count) return;
    lv_draw_task_type_t type = lv_draw_task_get_type(task);
    if (type != LV_DRAW_TASK_TYPE_FILL && type != LV_DRAW_TASK_TYPE_LABEL) return;

    lv_obj_t *table = lv_event_get_current_target(e);
    const row_t *row = &s_list.rows[base->id1];
    const theme_colors_t *clr = theme_get_colors();
    uint32_t sel_row = 0, sel_col = 0;
    lv_table_get_selected_cell(table, &sel_row, &sel_col);
    bool pressed = lv_obj_has_state(table, LV_STATE_PRESSED) && !lv_obj_has_state(table, LV_STATE_SCROLLED);
    bool keyed = s_list.keyed && lv_obj_has_state(table, LV_STATE_FOCUSED);
    bool chosen = row->kind != ROW_NOTE && base->id1 == sel_row && (pressed || keyed);

    lv_color_t bg = row->kind == ROW_NOTE ? clr->bg : clr->surface;
    lv_color_t fg = row->kind == ROW_NOTE ? clr->text_secondary : clr->text;
    if (chosen || (row->kind == ROW_CALL && row->unanswered && base->id2 == 0)) {
        bg = clr->primary;
        fg = clr->bg;
    }
    if (type == LV_DRAW_TASK_TYPE_FILL) {
        lv_draw_task_get_fill_dsc(task)->color = bg;
    } else {
        lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(task);
        label->color = fg;
        if (row->kind == ROW_CALL && base->id2 == 2) label->align = LV_TEXT_ALIGN_RIGHT;
    }
}

/* Favourites and Recents, which have the keyboard: the first Up or Down
 * shows the first row chosen, rather than moving past it. */
static void list_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key != LV_KEY_UP && key != LV_KEY_DOWN) return;
    if (!s_list.keyed) {
        int32_t first = first_selectable();
        if (first >= 0) select_row((uint32_t)first);
        s_list.keyed = true;
    }
    lv_obj_invalidate(s_list.table);
}

static void list_changed_cb(lv_event_t *e)
{
    if (s_list.filling) return;
    /* The selection also moves with the arrow keys, which must not open. */
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD && lv_indev_get_key(indev) != LV_KEY_ENTER) {
        return;
    }
    uint32_t row = 0, col = 0;
    lv_table_get_selected_cell(lv_event_get_target(e), &row, &col);
    if (row != LV_TABLE_CELL_NONE) open_row(row);
}

static lv_obj_t *make_list(lv_obj_t *page, int32_t y, uint32_t columns)
{
    lv_obj_t *table = phone_ui_table(page, y, phone_ui_page_height() - y, columns);
    lv_obj_add_event_cb(table, list_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(table, list_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(table, list_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_flag(table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    s_list.table = table;
    return table;
}

/* What an empty list says, in the table's place, with a button if given. */
static void make_empty(lv_obj_t *page, int32_t y, const char *button, lv_event_cb_t clicked)
{
    const phone_styles_t *st = phone_ui_styles();
    lv_obj_t *box = phone_ui_box(page);
    int32_t h = phone_ui_page_height() - y;
    lv_obj_set_pos(box, 0, y);
    lv_obj_set_size(box, phone_ui_width(), h > 1 ? h : 1);
    lv_obj_set_style_pad_all(box, PHONE_PAD * 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(box, PHONE_PAD * 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);

    s_list.empty_note = phone_ui_label(box, &st->note, "");
    lv_label_set_long_mode(s_list.empty_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_list.empty_note, LV_PCT(100));
    if (button) {
        s_list.rescan = phone_ui_button(box, button, &st->small, false, clicked, NULL);
        phone_ui_group_add(s_list.rescan);
    }
    s_list.empty = box;
}

/* The table, or the empty list's note in its place. */
static void show_empty(bool empty, const char *note)
{
    if (!s_list.empty) return;
    if (empty) phone_ui_set_text(s_list.empty_note, note);
    if (lv_obj_has_flag(s_list.empty, LV_OBJ_FLAG_HIDDEN) == empty) {
        if (empty) {
            lv_obj_remove_flag(s_list.empty, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_list.table, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_list.empty, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_list.table, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* A contact's row: the name, cut to fit, with a star if starred. */
static void contact_cell(const pb_contact_t *c, int32_t width, bool star)
{
    const lv_font_t *font = phone_ui_font();
    int32_t room = width - 2 * PHONE_CELL_PAD - 4;
    if (star && c->starred) room -= lv_text_get_width(" " PHONE_STAR, sizeof(" " PHONE_STAR) - 1, font, 0);
    phone_ui_fit(c->name, font, room, s_cell, sizeof s_cell);
    if (star && c->starred) {
        size_t used = strlen(s_cell);
        snprintf(s_cell + used, sizeof s_cell - used, " " PHONE_STAR);
    }
}

/* ------------------------------------------------------------------ */
/* Contacts                                                             */
/* ------------------------------------------------------------------ */

static void fill_contacts(void)
{
    if (!s_list.table || !s_list.search) return;
    s_list.book_seq = rs_phonebook_seq();
    s_list.loading = rs_phonebook_loading();
    uint32_t total = rs_phonebook_count();

    char placeholder[48];
    if (total) snprintf(placeholder, sizeof placeholder, "Search %u contact%s", (unsigned)total, total == 1 ? "" : "s");
    else snprintf(placeholder, sizeof placeholder, "Search");
    const char *had = lv_textarea_get_placeholder_text(s_list.search);
    if (!had || strcmp(had, placeholder) != 0) lv_textarea_set_placeholder_text(s_list.search, placeholder);

    unkey();
    rows_begin();
    if (!total) {
        rows_end();
        show_empty(true, s_list.loading ? "Looking for contacts" PHONE_ELLIPSIS
                                         : "No contacts. Put a contacts.vcf file on the SD card, in any folder.");
        if (s_list.rescan) {
            if (s_list.loading) lv_obj_add_flag(s_list.rescan, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(s_list.rescan, LV_OBJ_FLAG_HIDDEN);
        }
        ESP_LOGI(TAG, "contacts: 0 shown of 0%s", s_list.loading ? ", looking" : "");
        return;
    }
    show_empty(false, NULL);

    const char *query = lv_textarea_get_text(s_list.search);
    uint32_t matched = rs_phonebook_search(query, s_indices, PHONE_LIST_MAX);
    uint32_t shown = matched < PHONE_LIST_MAX ? matched : PHONE_LIST_MAX;
    int32_t width = phone_ui_width();
    for (uint32_t i = 0; i < shown; i++) {
        if (!rs_phonebook_get(s_indices[i], &s_contact)) continue;
        contact_cell(&s_contact, width, true);
        add_row(ROW_CONTACT, s_indices[i], 0, s_cell);
    }
    if (matched > shown) {
        snprintf(s_cell, sizeof s_cell, "%u more " PHONE_DASH " type to narrow", (unsigned)(matched - shown));
        add_row(ROW_NOTE, 0, 0, s_cell);
    } else if (!matched) {
        char q[QUERY_MAX];
        phone_ui_fit(query, phone_ui_font(), width / 2, q, sizeof q);
        snprintf(s_cell, sizeof s_cell, "No contact matches \xE2\x80\x9C%s\xE2\x80\x9D.", q);
        add_row(ROW_NOTE, 0, 0, s_cell);
    }
    rows_end();
    lv_obj_scroll_to_y(s_list.table, 0, LV_ANIM_OFF);
    ESP_LOGI(TAG, "contacts: %u shown of %u", (unsigned)shown, (unsigned)matched);
}

static void search_changed_cb(lv_event_t *e)
{
    (void)e;
    if (s_list.filling) return;
    fill_contacts();
}

static void search_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_DOWN) move_highlight(1);
    else if (key == LV_KEY_UP) move_highlight(-1);
}

/* Enter: the highlighted contact, or the first that matched. */
static void search_ready_cb(lv_event_t *e)
{
    (void)e;
    int32_t row = -1;
    if (s_list.keyed) {
        uint32_t r = 0, c = 0;
        lv_table_get_selected_cell(s_list.table, &r, &c);
        if (r != LV_TABLE_CELL_NONE) row = (int32_t)r;
    }
    if (row < 0) row = first_selectable();
    if (row >= 0 && selectable((uint32_t)row)) open_row((uint32_t)row);
}

static void rescan_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = rs_phonebook_rescan();
    ESP_LOGI(TAG, "search the card again: %s", err == ESP_OK ? "started" : "already searching");
    if (err == ESP_OK) toast_show("Searching the SD card", TOAST_INFO, 1500);
    fill_contacts();
}

static void build_contacts(lv_obj_t *page)
{
    const phone_styles_t *st = phone_ui_styles();
    int32_t w = phone_ui_width();

    lv_obj_t *ta = lv_textarea_create(page);
    lv_obj_remove_style_all(ta);
    lv_obj_add_style(ta, &st->field, LV_PART_MAIN);
    theme_style_text_field(ta);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, QUERY_MAX - 1);
    lv_textarea_set_placeholder_text(ta, "Search");
    lv_obj_set_pos(ta, PHONE_PAD / 2, PHONE_PAD / 2);
    lv_obj_set_width(ta, w - PHONE_PAD);
    s_list.filling = true;
    lv_textarea_set_text(ta, s_keep.query);
    s_list.filling = false;
    lv_obj_add_event_cb(ta, search_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ta, search_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(ta, search_ready_cb, LV_EVENT_READY, NULL);
    s_list.search = ta;
    lv_obj_update_layout(ta);
    int32_t y = PHONE_PAD / 2 + lv_obj_get_height(ta) + PHONE_PAD / 2;

    make_list(page, y, 1);
    make_empty(page, y, LV_SYMBOL_REFRESH "  Search the card again", rescan_cb);
    fill_contacts();
    restore_place();
    phone_ui_focus(ta);
}

/* ------------------------------------------------------------------ */
/* Favourites                                                           */
/* ------------------------------------------------------------------ */

static void fill_favourites(void)
{
    if (!s_list.table) return;
    unkey();
    s_list.book_seq = rs_phonebook_seq();
    s_list.loading = rs_phonebook_loading();
    uint32_t n = rs_phonebook_starred(s_indices, PHONE_LIST_MAX);
    uint32_t shown = n < PHONE_LIST_MAX ? n : PHONE_LIST_MAX;

    rows_begin();
    for (uint32_t i = 0; i < shown; i++) {
        if (!rs_phonebook_get(s_indices[i], &s_contact)) continue;
        contact_cell(&s_contact, phone_ui_width(), false);
        add_row(ROW_CONTACT, s_indices[i], 0, s_cell);
    }
    rows_end();
    show_empty(s_list.row_count == 0,
               s_list.loading && !rs_phonebook_count()
                   ? "Looking for contacts" PHONE_ELLIPSIS
                   : "No favourites yet. Open a contact and tap " PHONE_STAR_HOLLOW " to add one.");
    ESP_LOGI(TAG, "favourites: %u", (unsigned)s_list.row_count);
}

static void build_favourites(lv_obj_t *page)
{
    make_list(page, 0, 1);
    make_empty(page, 0, NULL, NULL);
    fill_favourites();
    restore_place();
    phone_ui_focus(s_list.table);
}

/* ------------------------------------------------------------------ */
/* Recents                                                              */
/* ------------------------------------------------------------------ */

static int local_day(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_year * 400 + tm.tm_yday;
}

static uint8_t call_class(uint8_t kind)
{
    /* A declined call is one received and not answered, as a missed one. */
    return kind == CALL_LOG_OUTGOING ? 0 : kind == CALL_LOG_INCOMING ? 1 : 2;
}

static void fill_recents(void)
{
    if (!s_list.table) return;
    unkey();
    s_list.log_seq = rs_call_log_seq();
    s_list.book_seq = rs_phonebook_seq();
    time_t now = time(NULL);
    s_list.day = local_day(now);
    time_status_t ts;
    bool h24 = time_manager_get_status(&ts) != ESP_OK || ts.format_24h;

    /* Calls one after another with one number, the same way, are one row,
     * as on a phone: "Anna Schmidt (3)". */
    uint32_t n = rs_call_log_count();
    if (n > CALL_LOG_KEEP) n = CALL_LOG_KEEP;
    call_log_entry_t entry, prev;
    memset(&prev, 0, sizeof prev);
    uint32_t rows = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!rs_call_log_get(i, &entry)) continue;
        if (rows && s_list.rows[rows - 1].count < 99 && call_class(entry.kind) == call_class(prev.kind) &&
            entry.number[0] && phone_text_same_number(entry.number, prev.number)) {
            s_list.rows[rows - 1].count++;
            continue;
        }
        row_t *r = &s_list.rows[rows];
        r->kind = ROW_CALL;
        r->index = (uint16_t)i;
        r->count = 1;
        r->unanswered = call_class(entry.kind) == 2;
        phone_text_when(entry.time, now, h24, s_whens[rows], sizeof s_whens[rows]);
        prev = entry;
        rows++;
    }

    /* Three columns: the mark, who, and when, as wide as its widest. */
    const lv_font_t *font = phone_ui_font();
    int32_t w = phone_ui_width();
    int32_t mark_w = lv_text_get_width(PHONE_ARROW_IN, sizeof(PHONE_ARROW_IN) - 1, font, 0) + 2 * MARK_PAD;
    int32_t when_w = 0;
    for (uint32_t r = 0; r < rows; r++) {
        int32_t tw = lv_text_get_width(s_whens[r], (uint32_t)strlen(s_whens[r]), font, 0);
        if (tw > when_w) when_w = tw;
    }
    when_w += 2 * MARK_PAD + 2;
    int32_t who_w = w - mark_w - when_w;
    if (who_w < 2 * MARK_PAD + 8) who_w = 2 * MARK_PAD + 8;
    lv_table_set_column_width(s_list.table, 0, mark_w);
    lv_table_set_column_width(s_list.table, 1, who_w);
    lv_table_set_column_width(s_list.table, 2, when_w);

    /* The rows' kinds are set; add_row() would set them again. */
    uint32_t count = rows;
    s_list.row_count = count;
    for (uint32_t r = 0; r < count; r++) {
        const row_t *row = &s_list.rows[r];
        if (!rs_call_log_get(row->index, &entry)) entry.number[0] = '\0';
        const char *who = entry.number[0] ? entry.number : "Unknown";
        if (entry.number[0] && rs_phonebook_name_for(entry.number, s_name, sizeof s_name)) who = s_name;
        char times[8] = "";
        if (row->count > 1) snprintf(times, sizeof times, " (%u)", (unsigned)row->count);
        int32_t room = who_w - 2 * MARK_PAD - lv_text_get_width(times, (uint32_t)strlen(times), font, 0);
        phone_ui_fit(who, font, room, s_cell, sizeof s_cell - sizeof times);
        strcat(s_cell, times);
        lv_table_set_cell_value(s_list.table, r, 0,
                                entry.kind == CALL_LOG_OUTGOING ? PHONE_ARROW_OUT : PHONE_ARROW_IN);
        lv_table_set_cell_value(s_list.table, r, 1, s_cell);
        lv_table_set_cell_value(s_list.table, r, 2, s_whens[r]);
    }
    rows_end();
    show_empty(count == 0, "No calls yet");
    if (s_list.clear) {
        if (count) lv_obj_remove_flag(s_list.clear, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_list.clear, LV_OBJ_FLAG_HIDDEN);
    }
    /* Seen: the status bar's count of missed calls goes. */
    if (rs_call_missed_count()) rs_call_missed_seen();
    ESP_LOGI(TAG, "recents: %u", (unsigned)count);
}

static void ask_clear(bool ask)
{
    if (!s_list.confirm) return;
    if (ask) {
        lv_obj_remove_flag(s_list.confirm, LV_OBJ_FLAG_HIDDEN);
        /* Its Cancel, for the keyboard: clearing takes a deliberate move. */
        phone_ui_focus(lv_obj_get_child(s_list.confirm, 1));
    } else {
        lv_obj_add_flag(s_list.confirm, LV_OBJ_FLAG_HIDDEN);
        phone_ui_focus(s_list.table);
    }
}

static void clear_cb(lv_event_t *e)
{
    (void)e;
    ask_clear(true);
}

static void clear_cancel_cb(lv_event_t *e)
{
    (void)e;
    ask_clear(false);
}

static void clear_confirm_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = rs_call_log_clear();
    ESP_LOGI(TAG, "recents: cleared%s", err == ESP_OK ? "" : ", not on the card");
    if (err != ESP_OK) toast_warn("Could not clear the calls on the SD card");
    ask_clear(false);
    fill_recents();
}

static void build_recents(lv_obj_t *page)
{
    const phone_styles_t *st = phone_ui_styles();
    int32_t w = phone_ui_width();
    int32_t bar_h = PHONE_BAR_H;
    if (bar_h > phone_ui_page_height() / 3) bar_h = phone_ui_page_height() / 3;

    lv_obj_t *bar = phone_ui_box(page);
    lv_obj_add_style(bar, &st->bar, LV_PART_MAIN);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, w, bar_h);
    lv_obj_t *title = phone_ui_label(bar, &st->medium, "Recents");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    s_list.clear = phone_ui_button(bar, "Clear", &st->small, false, clear_cb, NULL);
    lv_obj_set_height(s_list.clear, bar_h - 6);
    lv_obj_align(s_list.clear, LV_ALIGN_RIGHT_MID, 0, 0);

    /* The question, over the header when asked. */
    lv_obj_t *ask = phone_ui_box(page);
    lv_obj_add_style(ask, &st->bar, LV_PART_MAIN);
    lv_obj_set_pos(ask, 0, 0);
    lv_obj_set_size(ask, w, bar_h);
    lv_obj_add_flag(ask, LV_OBJ_FLAG_CLICKABLE);   /* taps stay in it */
    lv_obj_t *q = phone_ui_label(ask, &st->small, "Clear all calls?");
    lv_obj_align(q, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *cancel = phone_ui_button(ask, "Cancel", &st->small, false, clear_cancel_cb, NULL);
    lv_obj_t *yes = phone_ui_button(ask, "Clear", &st->small, true, clear_confirm_cb, NULL);
    lv_obj_set_height(cancel, bar_h - 6);
    lv_obj_set_height(yes, bar_h - 6);
    lv_obj_align(yes, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_update_layout(yes);
    lv_obj_align(cancel, LV_ALIGN_RIGHT_MID, -lv_obj_get_width(yes) - PHONE_GAP, 0);
    lv_obj_add_flag(ask, LV_OBJ_FLAG_HIDDEN);
    s_list.confirm = ask;

    make_list(page, bar_h, 3);
    lv_obj_set_style_pad_left(s_list.table, MARK_PAD, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(s_list.table, MARK_PAD, LV_PART_ITEMS);
    make_empty(page, bar_h, NULL, NULL);
    fill_recents();
    restore_place();
    phone_ui_group_add(s_list.clear);
    phone_ui_group_add(cancel);
    phone_ui_group_add(yes);
    phone_ui_focus(s_list.table);
}

/* ------------------------------------------------------------------ */
/* Opening a row                                                        */
/* ------------------------------------------------------------------ */

static void open_row(uint32_t row)
{
    if (!selectable(row)) return;
    const row_t *r = &s_list.rows[row];
    keep_place();
    if (r->kind == ROW_CONTACT) {
        phone_ui_open_contact(r->index);
    } else if (r->kind == ROW_CALL) {
        call_log_entry_t entry;
        if (!rs_call_log_get(r->index, &entry)) return;
        if (!entry.number[0]) {
            toast_info("The caller's number was withheld");
            return;
        }
        phone_ui_open_number(entry.number);
    }
}

/* ------------------------------------------------------------------ */
/* The frame's calls                                                    */
/* ------------------------------------------------------------------ */

void phone_lists_build(lv_obj_t *page, phone_tab_t tab)
{
    memset(&s_list, 0, sizeof s_list);
    s_list.tab = tab;
    switch (tab) {
    case PHONE_TAB_FAVOURITES: build_favourites(page); break;
    case PHONE_TAB_RECENTS:    build_recents(page); break;
    case PHONE_TAB_CONTACTS:   build_contacts(page); break;
    default: break;
    }
}

void phone_lists_tick(void)
{
    if (!s_list.table) return;
    bool book = rs_phonebook_seq() != s_list.book_seq || rs_phonebook_loading() != s_list.loading;
    switch (s_list.tab) {
    case PHONE_TAB_CONTACTS:
        if (book) fill_contacts();
        break;
    case PHONE_TAB_FAVOURITES:
        if (book) fill_favourites();
        break;
    case PHONE_TAB_RECENTS:
        /* Names come from the phonebook, and "Yesterday" changes at midnight. */
        if (book || rs_call_log_seq() != s_list.log_seq || local_day(time(NULL)) != s_list.day) {
            fill_recents();
        }
        break;
    default:
        break;
    }
}

void phone_lists_release(void)
{
    if (s_list.table && s_list.search) {
        snprintf(s_keep.query, sizeof s_keep.query, "%s", lv_textarea_get_text(s_list.search));
    }
    memset(&s_list, 0, sizeof s_list);
}

bool phone_lists_back(void)
{
    if (s_list.confirm && !lv_obj_has_flag(s_list.confirm, LV_OBJ_FLAG_HIDDEN)) {
        ask_clear(false);
        return true;
    }
    return false;
}

void phone_lists_top(void)
{
    if (!s_list.table) return;
    if (s_list.tab == PHONE_TAB_CONTACTS) unkey();
    lv_obj_scroll_to_y(s_list.table, 0, LV_ANIM_OFF);
}

void phone_lists_forget(void)
{
    memset(&s_keep, 0, sizeof s_keep);
    for (int i = 0; i < PHONE_TAB_COUNT; i++) s_keep.selected[i] = -1;
}
