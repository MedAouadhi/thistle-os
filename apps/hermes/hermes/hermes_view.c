/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Hermes's messages drawn in LVGL. See hermes_view.h.
 */
#include "hermes/hermes_view.h"
#include "hermes/hermes_glyphs.h"
#include "hermes/hermes_markdown.h"

#include "ui/theme.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST_INDENT  12
#define CELL_GAP     8

/* One document at a time: only the LVGL task draws, and it parses, draws
 * and is done with it before the next. */
static EXT_RAM_BSS_ATTR hermes_markdown_document_t s_doc;
/* A block's or a cell's spans put back together, before they go into an
 * arena. */
static EXT_RAM_BSS_ATTR char s_text[HERMES_MARKDOWN_TEXT_BYTES + 64];
/* A cell made drawable, to be measured; and a collapsed table row's lines.
 * Not on the stack: the LVGL task's is 8 KB. */
static EXT_RAM_BSS_ATTR char s_drawn[HERMES_MARKDOWN_TEXT_BYTES + 64];
static EXT_RAM_BSS_ATTR char s_group[2 * HERMES_MARKDOWN_TEXT_BYTES];

static hermes_view_styles_t s_styles;
static bool s_styles_made;

/* ------------------------------------------------------------------ */
/* Arenas                                                               */
/* ------------------------------------------------------------------ */

bool hermes_arena_init(hermes_arena_t *arena, size_t cap)
{
    arena->buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!arena->buf) arena->buf = malloc(cap);
    arena->cap = arena->buf ? cap : 0;
    arena->used = 0;
    return arena->buf != NULL;
}

void hermes_arena_free(hermes_arena_t *arena)
{
    free(arena->buf);
    arena->buf = NULL;
    arena->cap = arena->used = 0;
}

const char *hermes_arena_put(hermes_arena_t *arena, const char *text, size_t len)
{
    if (!arena->buf || arena->used + 2 > arena->cap) return NULL;
    char *at = arena->buf + arena->used;
    size_t n = hermes_glyphs(text, len, at, arena->cap - arena->used);
    arena->used += n + 1;
    return at;
}

/* ------------------------------------------------------------------ */
/* Styles                                                               */
/* ------------------------------------------------------------------ */

void hermes_view_styles_reset(void)
{
    if (!s_styles_made) return;
    lv_style_t *all = (lv_style_t *)&s_styles;
    for (size_t i = 0; i < sizeof(s_styles) / sizeof(lv_style_t); i++) lv_style_reset(&all[i]);
    s_styles_made = false;
}

void hermes_view_styles_init(void)
{
    hermes_view_styles_reset();
    lv_style_t *all = (lv_style_t *)&s_styles;
    for (size_t i = 0; i < sizeof(s_styles) / sizeof(lv_style_t); i++) lv_style_init(&all[i]);
    s_styles_made = true;
    const theme_colors_t *clr = theme_get_colors();

    lv_style_set_text_font(&s_styles.body, &lv_font_montserrat_14);
    lv_style_set_text_color(&s_styles.body, clr->text);

    lv_style_set_text_font(&s_styles.secondary, &lv_font_montserrat_14);
    lv_style_set_text_color(&s_styles.secondary, clr->text_secondary);

    lv_style_set_text_font(&s_styles.heading, &lv_font_montserrat_18);
    lv_style_set_text_color(&s_styles.heading, clr->text);

    lv_style_set_text_font(&s_styles.subheading, &lv_font_montserrat_14);
    lv_style_set_text_color(&s_styles.subheading, clr->text);
    lv_style_set_text_decor(&s_styles.subheading, LV_TEXT_DECOR_UNDERLINE);

    lv_style_set_text_font(&s_styles.code, &lv_font_montserrat_14);
    lv_style_set_text_color(&s_styles.code, clr->text);
    lv_style_set_bg_color(&s_styles.code, clr->surface);
    lv_style_set_bg_opa(&s_styles.code, LV_OPA_COVER);
    lv_style_set_border_color(&s_styles.code, clr->text_secondary);
    lv_style_set_border_width(&s_styles.code, 1);
    lv_style_set_pad_all(&s_styles.code, 4);

    lv_style_set_text_font(&s_styles.quote, &lv_font_montserrat_14);
    lv_style_set_text_color(&s_styles.quote, clr->text_secondary);
    lv_style_set_border_side(&s_styles.quote, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_color(&s_styles.quote, clr->text_secondary);
    lv_style_set_border_width(&s_styles.quote, 2);
    lv_style_set_pad_left(&s_styles.quote, 6);

    lv_style_set_bg_color(&s_styles.rule, clr->text_secondary);
    lv_style_set_bg_opa(&s_styles.rule, LV_OPA_COVER);
    lv_style_set_border_width(&s_styles.rule, 0);
    lv_style_set_radius(&s_styles.rule, 0);
    lv_style_set_pad_all(&s_styles.rule, 0);

    lv_style_set_text_font(&s_styles.cell_head, &lv_font_montserrat_14);
    lv_style_set_text_color(&s_styles.cell_head, clr->text);
    lv_style_set_text_decor(&s_styles.cell_head, LV_TEXT_DECOR_UNDERLINE);

    /* A row of a table, and any other box that only lays things out. */
    lv_style_set_bg_opa(&s_styles.row, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_styles.row, 0);
    lv_style_set_radius(&s_styles.row, 0);
    lv_style_set_pad_all(&s_styles.row, 0);
    lv_style_set_pad_column(&s_styles.row, CELL_GAP);
    lv_style_set_pad_row(&s_styles.row, 2);
    lv_style_set_layout(&s_styles.row, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_styles.row, LV_FLEX_FLOW_ROW);

    lv_style_set_bg_opa(&s_styles.box, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_styles.box, 0);
    lv_style_set_radius(&s_styles.box, 0);
    lv_style_set_pad_all(&s_styles.box, 0);
    lv_style_set_pad_row(&s_styles.box, 3);
    lv_style_set_layout(&s_styles.box, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_styles.box, LV_FLEX_FLOW_COLUMN);

    lv_style_set_bg_color(&s_styles.user, clr->surface);
    lv_style_set_bg_opa(&s_styles.user, LV_OPA_COVER);
    lv_style_set_border_color(&s_styles.user, clr->text);
    lv_style_set_border_width(&s_styles.user, 1);
    lv_style_set_radius(&s_styles.user, 4);
    lv_style_set_pad_all(&s_styles.user, 4);

    lv_style_set_bg_color(&s_styles.page, clr->bg);
    lv_style_set_bg_opa(&s_styles.page, LV_OPA_COVER);
    lv_style_set_border_width(&s_styles.page, 0);
    lv_style_set_radius(&s_styles.page, 0);
    lv_style_set_pad_all(&s_styles.page, 0);
    lv_style_set_text_color(&s_styles.page, clr->text);
    lv_style_set_text_font(&s_styles.page, &lv_font_montserrat_14);

    lv_style_set_bg_color(&s_styles.bar, clr->surface);
    lv_style_set_bg_opa(&s_styles.bar, LV_OPA_COVER);
    lv_style_set_border_color(&s_styles.bar, clr->text);
    lv_style_set_border_width(&s_styles.bar, 1);
    lv_style_set_radius(&s_styles.bar, 0);
    lv_style_set_pad_hor(&s_styles.bar, 4);
    lv_style_set_pad_ver(&s_styles.bar, 0);
    lv_style_set_pad_column(&s_styles.bar, 4);

    lv_style_set_bg_color(&s_styles.button, clr->bg);
    lv_style_set_bg_opa(&s_styles.button, LV_OPA_COVER);
    lv_style_set_border_color(&s_styles.button, clr->text);
    lv_style_set_border_width(&s_styles.button, 1);
    lv_style_set_radius(&s_styles.button, 4);
    lv_style_set_pad_hor(&s_styles.button, 8);
    lv_style_set_pad_ver(&s_styles.button, 5);
    lv_style_set_shadow_width(&s_styles.button, 0);
    lv_style_set_text_color(&s_styles.button, clr->text);
    lv_style_set_text_font(&s_styles.button, &lv_font_montserrat_14);

    /* E-paper has black and white only: pressed is inverted. */
    lv_style_set_bg_color(&s_styles.button_pressed, clr->text);
    lv_style_set_text_color(&s_styles.button_pressed, clr->bg);

    lv_style_set_border_side(&s_styles.button_inert, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_width(&s_styles.button_inert, 3);

    lv_style_set_bg_color(&s_styles.field, clr->bg);
    lv_style_set_bg_opa(&s_styles.field, LV_OPA_COVER);
    lv_style_set_border_color(&s_styles.field, clr->text);
    lv_style_set_border_width(&s_styles.field, 1);
    lv_style_set_radius(&s_styles.field, 4);
    lv_style_set_pad_hor(&s_styles.field, 4);
    lv_style_set_pad_ver(&s_styles.field, 5);
    lv_style_set_text_color(&s_styles.field, clr->text);

    lv_style_set_outline_width(&s_styles.focus, 2);
    lv_style_set_outline_color(&s_styles.focus, clr->text);
    lv_style_set_outline_pad(&s_styles.focus, 1);
}

const hermes_view_styles_t *hermes_view_styles(void)
{
    return &s_styles;
}

/* ------------------------------------------------------------------ */
/* Blocks                                                               */
/* ------------------------------------------------------------------ */

lv_obj_t *hermes_view_label(lv_obj_t *parent, hermes_arena_t *arena, const char *text,
                            size_t len, const lv_style_t *style)
{
    const char *kept = hermes_arena_put(arena, text, len);
    if (!kept) return NULL;
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_add_style(label, (lv_style_t *)style, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_text_static(label, kept);
    return label;
}

/* The spans first..first+count put together in s_text, after `prefix`. */
static size_t join_spans(uint8_t first, uint8_t count, const char *prefix)
{
    size_t n = (size_t)snprintf(s_text, sizeof(s_text), "%s", prefix ? prefix : "");
    for (uint8_t i = 0; i < count && first + i < s_doc.span_count; i++) {
        const hermes_markdown_span_t *span = &s_doc.spans[first + i];
        size_t len = span->length;
        if (n + len >= sizeof(s_text)) len = sizeof(s_text) - 1 - n;
        memcpy(s_text + n, s_doc.text + span->offset, len);
        n += len;
    }
    /* A code block's last line break, and any around a paragraph. */
    while (n > 0 && (s_text[n - 1] == '\n' || s_text[n - 1] == ' ')) n--;
    s_text[n] = '\0';
    return n;
}

static int32_t text_width(const char *text)
{
    lv_point_t size;
    lv_text_get_size(&size, text, &lv_font_montserrat_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

static void draw_table(lv_obj_t *parent, hermes_arena_t *arena, const hermes_markdown_block_t *table,
                       int32_t width)
{
    enum { COLUMNS = 12 };
    int32_t natural[COLUMNS] = { 0 };
    uint8_t columns = 0;
    const hermes_markdown_row_t *head = NULL;

    for (uint8_t r = 0; r < table->row_count; r++) {
        const hermes_markdown_row_t *row = &s_doc.rows[table->first_row + r];
        if (row->header && !head) head = row;
        for (uint8_t c = 0; c < row->cell_count && c < COLUMNS; c++) {
            const hermes_markdown_cell_t *cell = &s_doc.cells[row->first_cell + c];
            join_spans(cell->first_span, cell->span_count, NULL);
            hermes_glyphs(s_text, strlen(s_text), s_drawn, sizeof(s_drawn));
            int32_t w = text_width(s_drawn);
            if (w > natural[c]) natural[c] = w;
        }
        if (row->cell_count > columns) columns = row->cell_count > COLUMNS ? COLUMNS : row->cell_count;
    }
    int32_t total = 0;
    for (uint8_t c = 0; c < columns; c++) total += natural[c] + (c ? CELL_GAP : 0);

    const hermes_view_styles_t *st = &s_styles;
    if (total <= width) {
        /* A grid: one flex row a table row, each cell its natural width. */
        for (uint8_t r = 0; r < table->row_count; r++) {
            const hermes_markdown_row_t *row = &s_doc.rows[table->first_row + r];
            lv_obj_t *line = lv_obj_create(parent);
            lv_obj_remove_style_all(line);
            lv_obj_add_style(line, (lv_style_t *)&st->row, LV_PART_MAIN);
            lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
            for (uint8_t c = 0; c < row->cell_count && c < COLUMNS; c++) {
                const hermes_markdown_cell_t *cell = &s_doc.cells[row->first_cell + c];
                size_t n = join_spans(cell->first_span, cell->span_count, NULL);
                lv_obj_t *label = hermes_view_label(line, arena, s_text, n,
                                                    row->header ? &st->cell_head : &st->body);
                if (!label) return;
                lv_obj_set_width(label, natural[c] + 1);
                if (cell->align == HERMES_MARKDOWN_ALIGN_RIGHT) {
                    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
                } else if (cell->align == HERMES_MARKDOWN_ALIGN_CENTER) {
                    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
                }
            }
        }
        return;
    }

    /* Too wide: each body row as "Header: value" lines, a rule between. */
    bool first = true;
    for (uint8_t r = 0; r < table->row_count; r++) {
        const hermes_markdown_row_t *row = &s_doc.rows[table->first_row + r];
        if (row == head || row->header) continue;
        if (!first) {
            lv_obj_t *rule = lv_obj_create(parent);
            lv_obj_remove_style_all(rule);
            lv_obj_add_style(rule, (lv_style_t *)&st->rule, LV_PART_MAIN);
            lv_obj_set_size(rule, LV_PCT(100), 1);
        }
        first = false;
        char *group = s_group;
        size_t used = 0;
        for (uint8_t c = 0; c < row->cell_count && c < COLUMNS; c++) {
            const hermes_markdown_cell_t *cell = &s_doc.cells[row->first_cell + c];
            char name[96] = "";
            if (head && c < head->cell_count) {
                const hermes_markdown_cell_t *hc = &s_doc.cells[head->first_cell + c];
                size_t n = join_spans(hc->first_span, hc->span_count, NULL);
                if (n >= sizeof(name)) {
                    n = sizeof(name) - 1;
                    while (n > 0 && ((unsigned char)s_text[n] & 0xC0) == 0x80) n--;
                }
                memcpy(name, s_text, n);
                name[n] = '\0';
            }
            join_spans(cell->first_span, cell->span_count, NULL);
            int n = snprintf(group + used, sizeof(s_group) - used, "%s%s%s%s", used ? "\n" : "", name,
                             name[0] ? ": " : "", s_text);
            if (n < 0 || (size_t)n >= sizeof(s_group) - used) break;
            used += (size_t)n;
        }
        if (!hermes_view_label(parent, arena, group, used, &st->body)) return;
    }
}

void hermes_view_markdown(lv_obj_t *parent, hermes_arena_t *arena, const char *markdown,
                          int32_t width)
{
    const hermes_view_styles_t *st = &s_styles;
    if (!hermes_markdown_parse(markdown, strlen(markdown), &s_doc) || s_doc.block_count == 0) {
        /* Not Markdown md4c would take, or nothing in it: the text as it is. */
        hermes_view_label(parent, arena, markdown, strlen(markdown), &st->body);
        return;
    }
    for (uint8_t b = 0; b < s_doc.block_count; b++) {
        const hermes_markdown_block_t *block = &s_doc.blocks[b];
        size_t n;
        lv_obj_t *label = NULL;
        switch (block->kind) {
        case HERMES_MARKDOWN_BLOCK_HEADING:
            n = join_spans(block->first_span, block->span_count, NULL);
            label = hermes_view_label(parent, arena, s_text, n,
                                      block->heading_level <= 2 ? &st->heading : &st->subheading);
            break;
        case HERMES_MARKDOWN_BLOCK_LIST_ITEM: {
            char marker[16];
            if (block->ordered) snprintf(marker, sizeof(marker), "%u. ", (unsigned)block->ordinal);
            else snprintf(marker, sizeof(marker), "%s ", LV_SYMBOL_BULLET);
            n = join_spans(block->first_span, block->span_count, marker);
            label = hermes_view_label(parent, arena, s_text, n, &st->body);
            if (label && block->list_depth > 1) {
                lv_obj_set_style_pad_left(label, LIST_INDENT * (block->list_depth - 1), LV_PART_MAIN);
            }
            break;
        }
        case HERMES_MARKDOWN_BLOCK_CODE:
            n = join_spans(block->first_span, block->span_count, NULL);
            label = hermes_view_label(parent, arena, s_text, n, &st->code);
            break;
        case HERMES_MARKDOWN_BLOCK_QUOTE:
            n = join_spans(block->first_span, block->span_count, NULL);
            label = hermes_view_label(parent, arena, s_text, n, &st->quote);
            break;
        case HERMES_MARKDOWN_BLOCK_TABLE:
            draw_table(parent, arena, block, width);
            break;
        case HERMES_MARKDOWN_BLOCK_RULE: {
            lv_obj_t *rule = lv_obj_create(parent);
            lv_obj_remove_style_all(rule);
            lv_obj_add_style(rule, (lv_style_t *)&st->rule, LV_PART_MAIN);
            lv_obj_set_size(rule, LV_PCT(100), 1);
            break;
        }
        default:
            n = join_spans(block->first_span, block->span_count, NULL);
            label = hermes_view_label(parent, arena, s_text, n, &st->body);
            break;
        }
        (void)label;
    }
    if (s_doc.truncated) hermes_view_label(parent, arena, "...", 3, &st->secondary);
}
