/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — To-do list screen
 *
 * One page: a field to type a task into, and the tasks beneath it, newest
 * first.
 *
 *   ┌──────────────────────────────┐
 *   │ To-do   2 left  [Clear done] │  Clear done while anything is
 *   │ ┌──────────────────────────┐ │  ticked off
 *   │ │ Add a task               │ │  has the keyboard from the start
 *   │ └──────────────────────────┘ │
 *   │ ( )  Buy milk                │
 *   │ ( )  Call the plumber about  │
 *   │      the kitchen tap         │
 *   │ (v)  Pay the rent            │  ticked off: struck through
 *   └──────────────────────────────┘
 *
 * Typing and Enter adds a task, and the field keeps the keyboard for the
 * next one. A tap on a task ticks it off, or back on; holding it removes it.
 * From the keyboard, Tab or Down reaches the tasks, Up and Down move through
 * them, Enter or Space ticks one off, Backspace removes it, and typing goes
 * back to the field.
 *
 * The tasks are one lv_table, as the Phone app's lists are: a row costs its
 * text in LVGL's pool, where a row of widgets cost over a kilobyte. Its tick
 * box and strikethrough are drawn over the table's own drawing of the row.
 *
 * Each change is saved as it is made, to a Markdown checklist on the SD card
 * (todo_core.h) that File Transfer's page can edit too.
 */
#include "todo/todo_app.h"
#include "todo/todo_core.h"

#include "hal/board.h"
#include "hal/sdcard_path.h"
#include "ui/theme.h"
#include "ui/toast.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "todo";

#define TODO_PATH THISTLE_SDCARD "/todo.md"
/* Written first and renamed over the list, so a save cut short leaves one
 * of the two whole. */
#define TODO_TEMP THISTLE_SDCARD "/todo.md.tmp"

#define BAR_H    30
#define EDGE     6    /* the field's and the rows' distance from the screen's sides */
#define ROW_PAD  6    /* inside a row, either side */
#define ROW_VPAD 8    /* inside a row, above and below its text */
#define BOX      20   /* the tick box, its middle level with the first line's */
#define BOX_GAP  10   /* between the box and the text */
#define LINE_H   16   /* Montserrat 14's */

/* The list and its file's text, which is read and written whole: 32 KB, in
 * PSRAM while the app is open. */
typedef struct {
    todo_list_t list;
    char        file[TODO_FILE_MAX];
} store_t;

static struct {
    lv_obj_t *root;
    lv_obj_t *count;
    lv_obj_t *field;
    lv_obj_t *table;   /* row i is task i */
    lv_obj_t *empty;   /* in the table's place while there are no tasks */
    lv_obj_t *clear;
    store_t  *store;
    bool      epaper;
    bool      filling;   /* the table's selection is being set, not chosen */
    bool      save_failed;
} s_ui;

static struct {
    lv_style_t page;
    lv_style_t bar;
    lv_style_t note;
    lv_style_t well;
    lv_style_t field;
    lv_style_t field_focused;
    lv_style_t placeholder;
    lv_style_t cursor;
    lv_style_t table;
    lv_style_t cell;
    lv_style_t scrollbar;
    lv_style_t empty;
    lv_style_t empty_title;
    lv_style_t empty_hint;
    lv_style_t button;
    lv_style_t button_pressed;
    lv_style_t focus_ring;
} s_styles;
static bool s_styles_made;

/* ------------------------------------------------------------------ */
/* Styles                                                               */
/* ------------------------------------------------------------------ */

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
    lv_style_set_text_color(&s_styles.page, clr->text);
    lv_style_set_text_font(&s_styles.page, &lv_font_montserrat_14);

    lv_style_set_bg_color(&s_styles.bar, clr->surface);
    lv_style_set_bg_opa(&s_styles.bar, LV_OPA_COVER);
    lv_style_set_border_side(&s_styles.bar, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&s_styles.bar, clr->text);
    lv_style_set_border_width(&s_styles.bar, 1);
    lv_style_set_radius(&s_styles.bar, 0);
    lv_style_set_pad_hor(&s_styles.bar, 8);
    lv_style_set_pad_ver(&s_styles.bar, 0);
    lv_style_set_pad_column(&s_styles.bar, 8);
    lv_style_set_text_font(&s_styles.bar, &lv_font_montserrat_18);

    lv_style_set_text_color(&s_styles.note, clr->text_secondary);
    lv_style_set_text_font(&s_styles.note, &lv_font_montserrat_14);

    lv_style_set_pad_hor(&s_styles.well, EDGE);
    lv_style_set_pad_top(&s_styles.well, 8);
    lv_style_set_pad_bottom(&s_styles.well, 4);

    lv_style_set_bg_color(&s_styles.field, clr->surface);
    lv_style_set_bg_opa(&s_styles.field, LV_OPA_COVER);
    lv_style_set_border_color(&s_styles.field, clr->text_secondary);
    lv_style_set_border_width(&s_styles.field, 1);
    lv_style_set_radius(&s_styles.field, 6);
    lv_style_set_pad_hor(&s_styles.field, ROW_PAD);
    lv_style_set_pad_ver(&s_styles.field, 7);
    lv_style_set_text_color(&s_styles.field, clr->text);

    /* An outline doubles the border without moving the text inside it. */
    lv_style_set_border_color(&s_styles.field_focused, clr->primary);
    lv_style_set_outline_color(&s_styles.field_focused, clr->primary);
    lv_style_set_outline_width(&s_styles.field_focused, 1);
    lv_style_set_outline_pad(&s_styles.field_focused, 0);

    lv_style_set_text_color(&s_styles.placeholder, clr->text_secondary);

    /* A bar before the next letter. It does not blink on e-paper, where each
     * blink would be a refresh. */
    lv_style_set_border_side(&s_styles.cursor, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_width(&s_styles.cursor, 2);
    lv_style_set_border_color(&s_styles.cursor, clr->text);
    lv_style_set_bg_opa(&s_styles.cursor, LV_OPA_TRANSP);
    lv_style_set_anim_duration(&s_styles.cursor, s_ui.epaper ? 0 : 500);

    lv_style_set_bg_opa(&s_styles.table, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_styles.table, 0);
    lv_style_set_radius(&s_styles.table, 0);
    lv_style_set_pad_hor(&s_styles.table, EDGE);
    lv_style_set_pad_top(&s_styles.table, 2);
    lv_style_set_pad_bottom(&s_styles.table, 4);

    /* A row's background is drawn even where it is the page's, as the box
     * is drawn when it is (table_draw_cb). */
    lv_style_set_bg_color(&s_styles.cell, clr->bg);
    lv_style_set_bg_opa(&s_styles.cell, LV_OPA_COVER);
    lv_style_set_border_width(&s_styles.cell, 0);
    lv_style_set_radius(&s_styles.cell, 6);
    lv_style_set_pad_left(&s_styles.cell, ROW_PAD + BOX + BOX_GAP);
    lv_style_set_pad_right(&s_styles.cell, ROW_PAD);
    lv_style_set_pad_ver(&s_styles.cell, ROW_VPAD);
    lv_style_set_text_color(&s_styles.cell, clr->text);
    lv_style_set_text_font(&s_styles.cell, &lv_font_montserrat_14);

    lv_style_set_bg_color(&s_styles.scrollbar, clr->primary);
    lv_style_set_bg_opa(&s_styles.scrollbar, LV_OPA_COVER);
    lv_style_set_width(&s_styles.scrollbar, 2);
    lv_style_set_radius(&s_styles.scrollbar, 0);

    lv_style_set_bg_opa(&s_styles.empty, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_styles.empty, 0);
    lv_style_set_pad_hor(&s_styles.empty, 12);
    lv_style_set_pad_top(&s_styles.empty, 28);
    lv_style_set_pad_row(&s_styles.empty, 6);

    lv_style_set_text_align(&s_styles.empty_title, LV_TEXT_ALIGN_CENTER);
    lv_style_set_text_font(&s_styles.empty_title, &lv_font_montserrat_18);
    lv_style_set_text_color(&s_styles.empty_title, clr->text);

    lv_style_set_text_align(&s_styles.empty_hint, LV_TEXT_ALIGN_CENTER);
    lv_style_set_text_color(&s_styles.empty_hint, clr->text_secondary);

    lv_style_set_bg_color(&s_styles.button, clr->bg);
    lv_style_set_bg_opa(&s_styles.button, LV_OPA_COVER);
    lv_style_set_border_color(&s_styles.button, clr->text);
    lv_style_set_border_width(&s_styles.button, 1);
    lv_style_set_radius(&s_styles.button, 4);
    lv_style_set_pad_hor(&s_styles.button, 6);
    lv_style_set_pad_ver(&s_styles.button, 3);
    lv_style_set_shadow_width(&s_styles.button, 0);
    lv_style_set_text_color(&s_styles.button, clr->text);
    lv_style_set_text_font(&s_styles.button, &lv_font_montserrat_14);

    /* E-paper has black and white only: pressed is inverted. */
    lv_style_set_bg_color(&s_styles.button_pressed, clr->text);
    lv_style_set_text_color(&s_styles.button_pressed, clr->bg);

    lv_style_set_outline_width(&s_styles.focus_ring, 2);
    lv_style_set_outline_color(&s_styles.focus_ring, clr->primary);
    lv_style_set_outline_pad(&s_styles.focus_ring, 1);
}

/* ------------------------------------------------------------------ */
/* The file                                                             */
/* ------------------------------------------------------------------ */

static void load(void)
{
    store_t *st = s_ui.store;
    st->list.count = 0;
    FILE *f = fopen(TODO_PATH, "r");
    /* Only a save cut short between its two steps leaves the second. */
    if (!f) f = fopen(TODO_TEMP, "r");
    if (!f) {
        ESP_LOGI(TAG, "no list yet");
        return;
    }
    size_t n = fread(st->file, 1, sizeof st->file - 1, f);
    fclose(f);
    st->file[n] = '\0';
    todo_parse(&st->list, st->file);
    ESP_LOGI(TAG, "%d tasks, %d to do", st->list.count, todo_open_count(&st->list));
}

static void save(void)
{
    store_t *st = s_ui.store;
    size_t n = todo_format(&st->list, st->file, sizeof st->file);

    FILE *f = fopen(TODO_TEMP, "w");
    bool ok = f && fwrite(st->file, 1, n, f) == n;
    if (f && fclose(f) != 0) ok = false;
    if (ok) {
        /* FAT renames over nothing. */
        remove(TODO_PATH);
        ok = rename(TODO_TEMP, TODO_PATH) == 0;
    }
    if (!ok) {
        ESP_LOGW(TAG, "could not save %s", TODO_PATH);
        if (!s_ui.save_failed) toast_warn("Could not save to the SD card");
    }
    s_ui.save_failed = !ok;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                              */
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

static void focus(lv_obj_t *obj)
{
    if (lv_group_get_default() && obj) lv_group_focus_obj(obj);
}

static bool has_focus(lv_obj_t *obj)
{
    lv_group_t *group = lv_group_get_default();
    return group && lv_group_get_focused(group) == obj;
}

/* The task the table has selected, or -1. */
static int selected_row(void)
{
    uint32_t row = LV_TABLE_CELL_NONE, col = LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(s_ui.table, &row, &col);
    return row < (uint32_t)s_ui.store->list.count ? (int)row : -1;
}

static void select_row(int row)
{
    s_ui.filling = true;
    lv_table_set_selected_cell(s_ui.table, (uint16_t)row, 0);
    s_ui.filling = false;
}

/* The table draws each row's background and text; this sets their colours
 * and draws the tick box over the background. A row is shown chosen while
 * the table has the keyboard, which only the keys give it. */
static void table_draw_cb(lv_event_t *e)
{
    lv_draw_task_t *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(task);
    const todo_list_t *list = &s_ui.store->list;
    if (base->part != LV_PART_ITEMS || base->id1 >= (uint32_t)list->count) return;
    lv_draw_task_type_t type = lv_draw_task_get_type(task);
    if (type != LV_DRAW_TASK_TYPE_FILL && type != LV_DRAW_TASK_TYPE_LABEL) return;

    const theme_colors_t *clr = theme_get_colors();
    bool done = list->items[base->id1].done;
    bool here = (int)base->id1 == selected_row();
    bool chosen = here && lv_obj_has_state(s_ui.table, LV_STATE_FOCUSED);
    bool pressed = here && lv_obj_has_state(s_ui.table, LV_STATE_PRESSED) &&
                   !lv_obj_has_state(s_ui.table, LV_STATE_SCROLLED);

    if (type == LV_DRAW_TASK_TYPE_LABEL) {
        lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(task);
        label->color = chosen ? clr->bg : done ? clr->text_secondary : clr->text;
        if (done) label->decor = LV_TEXT_DECOR_STRIKETHROUGH;
        return;
    }

    /* A pressed row is the surface colour: white on e-paper, so a tap there
     * redraws nothing but the tick. */
    lv_draw_task_get_fill_dsc(task)->color = chosen    ? clr->primary
                                             : pressed ? clr->surface
                                                       : clr->bg;

    lv_area_t box;
    lv_draw_task_get_area(task, &box);
    box.x1 += ROW_PAD;
    box.y1 += ROW_VPAD + LINE_H / 2 - BOX / 2;
    box.x2 = box.x1 + BOX - 1;
    box.y2 = box.y1 + BOX - 1;

    /* On a chosen row, whose background is the primary colour, the box is
     * in the page's. */
    lv_color_t ink = chosen ? clr->bg : clr->primary;
    lv_draw_rect_dsc_t ring;
    lv_draw_rect_dsc_init(&ring);
    ring.radius = LV_RADIUS_CIRCLE;
    ring.border_width = 2;
    ring.border_color = done || chosen ? ink : clr->text_secondary;
    ring.bg_color = ink;
    ring.bg_opa = done ? LV_OPA_COVER : LV_OPA_TRANSP;
    lv_draw_rect(base->layer, &ring, &box);
    if (!done) return;

    lv_draw_label_dsc_t tick;
    lv_draw_label_dsc_init(&tick);
    tick.text = LV_SYMBOL_OK;
    tick.font = &lv_font_montserrat_14;
    tick.color = chosen ? clr->primary : clr->bg;
    tick.align = LV_TEXT_ALIGN_CENTER;
    lv_area_increase(&box, -2, -2);
    lv_draw_label(base->layer, &tick, &box);
}

/* "2 left" in the title bar, and Clear done beside it while anything is
 * ticked off. */
static void show_counts(void)
{
    const todo_list_t *list = &s_ui.store->list;
    int open = todo_open_count(list);
    int done = list->count - open;
    char text[32];

    if (list->count == 0) text[0] = '\0';
    else if (open == 0) snprintf(text, sizeof text, "All done");
    else snprintf(text, sizeof text, "%d left", open);
    set_text(s_ui.count, text);

    if (done == 0 && has_focus(s_ui.clear)) focus(s_ui.field);
    set_shown(s_ui.clear, done > 0);
}

/* The table's rows from the list, after a task is added or removed. */
static void fill(void)
{
    const todo_list_t *list = &s_ui.store->list;
    s_ui.filling = true;
    lv_table_set_row_count(s_ui.table, list->count ? (uint32_t)list->count : 1);
    if (!list->count) lv_table_set_cell_value(s_ui.table, 0, 0, "");
    for (int i = 0; i < list->count; i++) {
        lv_table_set_cell_value(s_ui.table, (uint32_t)i, 0, list->items[i].text);
    }
    s_ui.filling = false;

    if (!list->count && has_focus(s_ui.table)) focus(s_ui.field);
    set_shown(s_ui.table, list->count > 0);
    set_shown(s_ui.empty, list->count == 0);
    show_counts();
}

/* ------------------------------------------------------------------ */
/* Changes                                                              */
/* ------------------------------------------------------------------ */

static void toggle(int row)
{
    todo_list_t *list = &s_ui.store->list;
    todo_toggle(list, row);
    lv_obj_invalidate(s_ui.table);
    show_counts();
    save();
    ESP_LOGI(TAG, "%s; %d to do", list->items[row].done ? "ticked off" : "not done after all",
             todo_open_count(list));
}

static void remove_task(int row)
{
    todo_list_t *list = &s_ui.store->list;
    todo_remove(list, row);
    save();
    ESP_LOGI(TAG, "removed; %d tasks, %d to do", list->count, todo_open_count(list));
    fill();
    /* From the keys, the next task takes the keyboard. */
    if (list->count && has_focus(s_ui.table)) {
        select_row(row < list->count ? row : list->count - 1);
    }
}

static void clear_cb(lv_event_t *e)
{
    (void)e;
    todo_list_t *list = &s_ui.store->list;
    int cleared = todo_clear_done(list);
    if (cleared == 0) return;
    save();
    ESP_LOGI(TAG, "cleared %d done; %d to do", cleared, list->count);
    fill();
    if (list->count && has_focus(s_ui.table)) select_row(0);
}

static void field_ready_cb(lv_event_t *e)
{
    (void)e;
    todo_list_t *list = &s_ui.store->list;
    switch (todo_add(list, lv_textarea_get_text(s_ui.field))) {
    case TODO_EMPTY:
        return;
    case TODO_FULL:
        toast_warn("The list is full: clear what is done");
        return;
    case TODO_ADDED:
        break;
    }
    lv_textarea_set_text(s_ui.field, "");
    save();
    ESP_LOGI(TAG, "added; %d tasks, %d to do", list->count, todo_open_count(list));
    fill();
    lv_obj_scroll_to_y(s_ui.table, 0, LV_ANIM_OFF);
}

static void field_key_cb(lv_event_t *e)
{
    lv_group_t *group = lv_group_get_default();
    if (group && lv_event_get_key(e) == LV_KEY_DOWN) lv_group_focus_next(group);
}

/* Up from the first task goes back to the field, before the table, which
 * would keep the first, sees the key. */
static void table_up_cb(lv_event_t *e)
{
    if (lv_event_get_key(e) != LV_KEY_UP || selected_row() > 0) return;
    focus(s_ui.field);
    lv_event_stop_processing(e);
}

static void table_cb(lv_event_t *e)
{
    int row = selected_row();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_FOCUSED:
        if (row < 0) select_row(0);
        lv_obj_invalidate(s_ui.table);
        break;
    case LV_EVENT_DEFOCUSED:
        lv_obj_invalidate(s_ui.table);
        break;
    case LV_EVENT_VALUE_CHANGED: {
        /* A tap, or Enter; not the selection moving with Up and Down. */
        if (s_ui.filling || row < 0) return;
        lv_indev_t *indev = lv_indev_active();
        if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD &&
            lv_indev_get_key(indev) != LV_KEY_ENTER) {
            return;
        }
        toggle(row);
        break;
    }
    case LV_EVENT_LONG_PRESSED:
        if (row < 0) return;
        /* Or letting go would tick off the task taking this one's place.
         * Held Enter is then let go unseen, so the table is pressed no
         * longer from now. */
        lv_indev_wait_release(lv_indev_active());
        lv_obj_remove_state(s_ui.table, LV_STATE_PRESSED);
        remove_task(row);
        break;
    case LV_EVENT_KEY: {
        uint32_t key = lv_event_get_key(e);
        if (key > ' ' && key < 0x7F) {
            /* Typing goes to the field, wherever the keyboard was. */
            focus(s_ui.field);
            lv_textarea_add_char(s_ui.field, key);
        } else if (row < 0) {
            return;
        } else if (key == ' ') {
            toggle(row);
        } else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
            remove_task(row);
        }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Making and unmaking                                                  */
/* ------------------------------------------------------------------ */

static void make_field(lv_obj_t *parent)
{
    lv_obj_t *well = lv_obj_create(parent);
    lv_obj_remove_style_all(well);
    lv_obj_add_style(well, &s_styles.well, LV_PART_MAIN);
    lv_obj_set_size(well, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(well, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.field = lv_textarea_create(well);
    lv_obj_remove_style_all(s_ui.field);
    lv_obj_add_style(s_ui.field, &s_styles.field, LV_PART_MAIN);
    lv_obj_add_style(s_ui.field, &s_styles.field_focused, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_add_style(s_ui.field, &s_styles.placeholder, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_style(s_ui.field, &s_styles.cursor, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_textarea_set_one_line(s_ui.field, true);
    lv_textarea_set_max_length(s_ui.field, TODO_CHARS);
    lv_textarea_set_placeholder_text(s_ui.field, "Add a task");
    lv_obj_set_width(s_ui.field, LV_PCT(100));
    lv_obj_add_event_cb(s_ui.field, field_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_ui.field, field_key_cb, LV_EVENT_KEY, NULL);
    lv_group_t *group = lv_group_get_default();
    if (group) lv_group_add_obj(group, s_ui.field);
}

static void make_table(lv_obj_t *parent, int32_t width)
{
    s_ui.table = lv_table_create(parent);
    lv_obj_remove_style_all(s_ui.table);
    lv_obj_add_style(s_ui.table, &s_styles.table, LV_PART_MAIN);
    lv_obj_add_style(s_ui.table, &s_styles.cell, LV_PART_ITEMS);
    lv_obj_add_style(s_ui.table, &s_styles.scrollbar, LV_PART_SCROLLBAR);
    lv_obj_set_width(s_ui.table, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.table, 1);
    lv_obj_set_scrollbar_mode(s_ui.table, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_ui.table, LV_DIR_VER);
    /* A tap leaves the keyboard in the field, for the next task. */
    lv_obj_remove_flag(s_ui.table, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_table_set_column_count(s_ui.table, 1);
    lv_table_set_column_width(s_ui.table, 0, width - 2 * EDGE);

    lv_obj_add_flag(s_ui.table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(s_ui.table, table_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_event_cb(s_ui.table, table_up_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, NULL);
    lv_obj_add_event_cb(s_ui.table, table_cb, LV_EVENT_ALL, NULL);
    lv_group_t *group = lv_group_get_default();
    if (group) lv_group_add_obj(group, s_ui.table);
}

static void make_empty(lv_obj_t *parent)
{
    s_ui.empty = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.empty);
    lv_obj_add_style(s_ui.empty, &s_styles.empty, LV_PART_MAIN);
    lv_obj_set_width(s_ui.empty, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.empty, 1);
    lv_obj_set_flex_flow(s_ui.empty, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_ui.empty, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.empty, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(s_ui.empty);
    lv_obj_add_style(title, &s_styles.empty_title, LV_PART_MAIN);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_text_static(title, "Nothing to do");

    lv_obj_t *hint = lv_label_create(s_ui.empty);
    lv_obj_add_style(hint, &s_styles.empty_hint, LV_PART_MAIN);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text_static(hint, "Type a task and press Enter.\n"
                                   "Tap it when it's done.\n"
                                   "Hold it to remove it.");
}

static void make_clear_button(lv_obj_t *parent)
{
    s_ui.clear = lv_button_create(parent);
    lv_obj_remove_style_all(s_ui.clear);
    lv_obj_add_style(s_ui.clear, &s_styles.button, LV_PART_MAIN);
    lv_obj_add_style(s_ui.clear, &s_styles.button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(s_ui.clear, &s_styles.focus_ring, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_remove_flag(s_ui.clear, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(s_ui.clear, clear_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_ui.clear, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *label = lv_label_create(s_ui.clear);
    lv_label_set_text_static(label, "Clear done");
    lv_obj_center(label);

    lv_group_t *group = lv_group_get_default();
    if (group) lv_group_add_obj(group, s_ui.clear);
}

esp_err_t todo_ui_create(lv_obj_t *parent)
{
    if (!parent) parent = lv_screen_active();
    memset(&s_ui, 0, sizeof s_ui);

    s_ui.store = heap_caps_malloc(sizeof *s_ui.store, MALLOC_CAP_SPIRAM);
    if (!s_ui.store) s_ui.store = malloc(sizeof *s_ui.store);
    if (!s_ui.store) return ESP_ERR_NO_MEM;
    load();

    const hal_registry_t *reg = hal_get_registry();
    s_ui.epaper = reg && reg->display && reg->display->type == HAL_DISPLAY_TYPE_EPAPER;
    styles_init();
    lv_obj_update_layout(parent);
    int32_t width = lv_obj_get_content_width(parent);
    if (width <= 0) width = 240;

    s_ui.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.root);
    lv_obj_add_style(s_ui.root, &s_styles.page, LV_PART_MAIN);
    lv_obj_set_size(s_ui.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_ui.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, &s_styles.bar, LV_PART_MAIN);
    lv_obj_set_size(bar, LV_PCT(100), BAR_H);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text_static(title, "To-do");
    lv_obj_set_flex_grow(title, 1);
    s_ui.count = lv_label_create(bar);
    lv_obj_add_style(s_ui.count, &s_styles.note, LV_PART_MAIN);
    lv_label_set_text(s_ui.count, "");

    /* In this order for Tab: the field, the tasks, Clear done. */
    make_field(s_ui.root);
    make_table(s_ui.root, width);
    make_empty(s_ui.root);
    make_clear_button(bar);

    fill();
    focus(s_ui.field);

    /* What the rows left of LVGL's pool (TODO_MAX). */
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "%d rows; LVGL pool %u KB free, largest block %u KB", s_ui.store->list.count,
             (unsigned)(mon.free_size / 1024), (unsigned)(mon.free_biggest_size / 1024));
    return ESP_OK;
}

void todo_ui_destroy(void)
{
    if (s_ui.root) lv_obj_delete(s_ui.root);
    free(s_ui.store);
    memset(&s_ui, 0, sizeof s_ui);
    styles_reset();
}
