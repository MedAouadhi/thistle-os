/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Arabic in every label and text field.
 *
 * Two things stood between Arabic and the screen. The fonts the apps use,
 * LVGL's Montserrat, have no Arabic, so each letter was drawn as a box. And
 * an Arabic letter takes a different form by where it stands in a word,
 * which LVGL does only with LV_USE_ARABIC_PERSIAN_CHARS, kept off
 * (arabic_shape.c says why). LV_USE_BIDI, on, already lays it out right to
 * left.
 *
 * Both are done here, beneath every widget, by wrapping LVGL's own functions
 * at link time (-Wl,--wrap, in components/ui/CMakeLists.txt and the
 * simulator's):
 *
 * - lv_font_get_glyph_dsc, lv_font_get_glyph_width: a character in the
 *   Arabic blocks that the font has no glyph for is taken from DejaVu Sans'
 *   Arabic at the size nearest the font's (src/fonts/, made by
 *   tools/gen_arabic_fonts.sh): one bit a pixel on e-paper, as the Reader's.
 * - lv_draw_label: text is drawn shaped, its selection moved with it.
 * - lv_text_get_size: and measured shaped, so a label is the size of what it
 *   shows, and breaks its lines where they are drawn breaking.
 * - lv_label_get_letter_pos, _get_letter_on, _is_char_under_pos: a text
 *   area's cursor, a tap on it and the up and down keys find their letter in
 *   the shaped text, where it is drawn.
 *
 * What a label or text area holds is never changed: an app reads back, saves
 * and sends the letters it was given, not their forms. The text is shaped
 * afresh for each of these calls, a cost only text with Arabic letters pays.
 * The Reader, Phone and Podcasts shape their own text before measuring it;
 * text already shaped is drawn as it is.
 *
 * A wrap catches calls from another file only. LVGL's label calling its own
 * functions is not caught, so the "..." of LV_LABEL_LONG_DOT goes where the
 * unjoined letters would end, a little early.
 */
#include "ui/arabic_text.h"
#include "ui/arabic_shape.h"

#include "hal/board.h"
#include "lvgl.h"
#include "src/misc/lv_text_private.h"           /* lv_text_encoded_get_byte_id() */
#include "src/widgets/label/lv_label_private.h" /* lv_label_t, for its text */
#include "esp_log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "arabic";

LV_FONT_DECLARE(arabic_font_14);
LV_FONT_DECLARE(arabic_font_14_mono);
LV_FONT_DECLARE(arabic_font_18);
LV_FONT_DECLARE(arabic_font_18_mono);
LV_FONT_DECLARE(arabic_font_22);
LV_FONT_DECLARE(arabic_font_22_mono);

/* LVGL's own, which the wraps call. */
bool __real_lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                                  uint32_t letter, uint32_t letter_next);
uint16_t __real_lv_font_get_glyph_width(const lv_font_t *font, uint32_t letter,
                                        uint32_t letter_next);
void __real_lv_draw_label(lv_layer_t *layer, const lv_draw_label_dsc_t *dsc,
                          const lv_area_t *coords);
void __real_lv_text_get_size(lv_point_t *size_res, const char *text, const lv_font_t *font,
                             int32_t letter_space, int32_t line_space, int32_t max_width,
                             lv_text_flag_t flag);
void __real_lv_label_get_letter_pos(const lv_obj_t *obj, uint32_t char_id, lv_point_t *pos);
uint32_t __real_lv_label_get_letter_on(const lv_obj_t *obj, lv_point_t *pos_in, bool bidi);
bool __real_lv_label_is_char_under_pos(const lv_obj_t *obj, lv_point_t *pos);

/* The wraps, which the linker sends LVGL's calls to. */
bool __wrap_lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                                  uint32_t letter, uint32_t letter_next);
uint16_t __wrap_lv_font_get_glyph_width(const lv_font_t *font, uint32_t letter,
                                        uint32_t letter_next);
void __wrap_lv_draw_label(lv_layer_t *layer, const lv_draw_label_dsc_t *dsc,
                          const lv_area_t *coords);
void __wrap_lv_text_get_size(lv_point_t *size_res, const char *text, const lv_font_t *font,
                             int32_t letter_space, int32_t line_space, int32_t max_width,
                             lv_text_flag_t flag);
void __wrap_lv_label_get_letter_pos(const lv_obj_t *obj, uint32_t char_id, lv_point_t *pos);
uint32_t __wrap_lv_label_get_letter_on(const lv_obj_t *obj, lv_point_t *pos_in, bool bidi);
bool __wrap_lv_label_is_char_under_pos(const lv_obj_t *obj, lv_point_t *pos);

/* ── The fonts ────────────────────────────────────────────────────────── */

/* By the line height of the font they stand in for: Montserrat 14, 18 and 22
 * have lines of 16, 21 and 24 px. */
static const lv_font_t *s_fonts[3] = {&arabic_font_14, &arabic_font_18, &arabic_font_22};

void arabic_text_init(lv_display_t *disp)
{
    const hal_registry_t *reg = hal_get_registry();
    if (reg && reg->display && reg->display->type == HAL_DISPLAY_TYPE_EPAPER) {
        s_fonts[0] = &arabic_font_14_mono;
        s_fonts[1] = &arabic_font_18_mono;
        s_fonts[2] = &arabic_font_22_mono;
    }

    /* LVGL lays every text out left to right unless told otherwise: a line
     * of Arabic then reads right to left, but its paragraph starts at the
     * left, and the end of its text, where the cursor goes, is at the right.
     * Auto takes each text's direction from its first letter that has one,
     * as the Reader does for a book in no stated language. Every widget
     * inherits it; containers lay out left to right under it as before. */
    if (!disp) return;
    lv_obj_t *roots[] = {lv_display_get_screen_active(disp), lv_display_get_layer_top(disp),
                         lv_display_get_layer_sys(disp)};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        if (roots[i]) lv_obj_set_style_base_dir(roots[i], LV_BASE_DIR_AUTO, LV_PART_MAIN);
    }
}

static const lv_font_t *arabic_font_for(const lv_font_t *font)
{
    int32_t h = font->line_height;
    return s_fonts[h < 19 ? 0 : h < 23 ? 1 : 2];
}

/* The blocks the Arabic fonts cover. */
static bool in_arabic_blocks(uint32_t c)
{
    return (c >= 0x0600 && c <= 0x06FF) || (c >= 0xFB50 && c <= 0xFBFF) ||
           (c >= 0xFE70 && c <= 0xFEFF);
}

/* Says once for each character of those blocks that no font has, which LVGL
 * draws as a box. */
static void note_missing(uint32_t c)
{
    static uint8_t s_noted[(0x100 + 0xB0 + 0x90) / 8];
    uint32_t bit = c <= 0x06FF ? c - 0x0600 : c <= 0xFBFF ? 0x100 + c - 0xFB50
                                                          : 0x1B0 + c - 0xFE70;
    if (s_noted[bit / 8] & (1u << (bit % 8))) return;
    s_noted[bit / 8] |= (uint8_t)(1u << (bit % 8));
    ESP_LOGW(TAG, "no glyph for U+%04X", (unsigned)c);
}

bool __wrap_lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                                  uint32_t letter, uint32_t letter_next)
{
    if (__real_lv_font_get_glyph_dsc(font, dsc, letter, letter_next)) return true;
    if (!font || !in_arabic_blocks(letter) || lv_text_is_marker(letter)) return false;

    const lv_font_t *arabic = arabic_font_for(font);
    lv_font_glyph_dsc_t g;
    if (arabic != font && __real_lv_font_get_glyph_dsc(arabic, &g, letter, letter_next)) {
        *dsc = g;
        return true;
    }
    note_missing(letter);
    return false;
}

/* LVGL's measures a glyph with its own lookup, which the linker does not
 * send here: it is in the same file. */
uint16_t __wrap_lv_font_get_glyph_width(const lv_font_t *font, uint32_t letter,
                                        uint32_t letter_next)
{
    if (!in_arabic_blocks(letter) || lv_text_is_marker(letter)) {
        return __real_lv_font_get_glyph_width(font, letter, letter_next);
    }
    lv_font_glyph_dsc_t g;
    __wrap_lv_font_get_glyph_dsc(font, &g, letter, letter_next);
    return g.adv_w;
}

/* ── Drawing and measuring shaped ─────────────────────────────────────── */

/* `text` shaped, to free(), or NULL when it needs no shaping or there is no
 * memory, either way to be used as it is. `offsets` as arabic_shape()'s. */
static char *shaped_copy(const char *text, size_t *offsets, size_t count, size_t *len_out)
{
    size_t len = text ? strlen(text) : 0;
    if (len == 0 || !arabic_needs_shaping(text, len)) return NULL;
    return arabic_shape(text, len, offsets, count, len_out);
}

void __wrap_lv_draw_label(lv_layer_t *layer, const lv_draw_label_dsc_t *dsc,
                          const lv_area_t *coords)
{
    /* The selection's ends are characters; moved as the bytes they begin. */
    bool selected = dsc && dsc->text && dsc->sel_start != LV_DRAW_LABEL_NO_TXT_SEL &&
                    dsc->sel_end != LV_DRAW_LABEL_NO_TXT_SEL;
    size_t ends[2] = {0, 0};
    if (selected) {
        ends[0] = lv_text_encoded_get_byte_id(dsc->text, dsc->sel_start);
        ends[1] = lv_text_encoded_get_byte_id(dsc->text, dsc->sel_end);
    }
    size_t len = 0;
    char *shaped = dsc ? shaped_copy(dsc->text, ends, selected ? 2 : 0, &len) : NULL;
    if (!shaped) {
        __real_lv_draw_label(layer, dsc, coords);
        return;
    }

    /* LVGL copies a text_local text into its 64 KB pool, and does not check
     * that it could: what does not fit is drawn as it is. */
    void *room = lv_malloc(len + 1);
    if (!room) {
        free(shaped);
        __real_lv_draw_label(layer, dsc, coords);
        return;
    }
    lv_free(room);

    lv_draw_label_dsc_t copy = *dsc;
    copy.text       = shaped;
    copy.text_local = 1;
    /* A long label's hint keeps where its lines begin, in its own text. */
    copy.hint       = NULL;
    if (selected) {
        copy.sel_start = lv_text_encoded_get_char_id(shaped, (uint32_t)ends[0]);
        copy.sel_end   = lv_text_encoded_get_char_id(shaped, (uint32_t)ends[1]);
    }
    __real_lv_draw_label(layer, &copy, coords);
    free(shaped);
}

void __wrap_lv_text_get_size(lv_point_t *size_res, const char *text, const lv_font_t *font,
                             int32_t letter_space, int32_t line_space, int32_t max_width,
                             lv_text_flag_t flag)
{
    char *shaped = shaped_copy(text, NULL, 0, NULL);
    __real_lv_text_get_size(size_res, shaped ? shaped : text, font, letter_space, line_space,
                            max_width, flag);
    free(shaped);
}

/* ── Finding a letter ─────────────────────────────────────────────────── */

/* A label's text, swapped for its shaped text while LVGL measures it, which
 * it does by reading the label's own. */
typedef struct {
    lv_label_t *label;
    char       *logical;
    char       *shaped;
    size_t      len;      /* the logical text's */
} swap_t;

static bool swap_in(const lv_obj_t *obj, swap_t *s, size_t *offsets, size_t count)
{
    lv_label_t *label = (lv_label_t *)obj;
    char *shaped = shaped_copy(label->text, offsets, count, NULL);
    if (!shaped) return false;
    s->label   = label;
    s->logical = label->text;
    s->shaped  = shaped;
    s->len     = strlen(label->text);
    label->text = shaped;
    return true;
}

static void swap_out(swap_t *s)
{
    s->label->text = s->logical;
    free(s->shaped);
}

void __wrap_lv_label_get_letter_pos(const lv_obj_t *obj, uint32_t char_id, lv_point_t *pos)
{
    const char *text = ((const lv_label_t *)obj)->text;
    size_t at = text ? lv_text_encoded_get_byte_id(text, char_id) : 0;
    swap_t s;
    if (!swap_in(obj, &s, &at, 1)) {
        __real_lv_label_get_letter_pos(obj, char_id, pos);
        return;
    }
    __real_lv_label_get_letter_pos(obj, lv_text_encoded_get_char_id(s.shaped, (uint32_t)at), pos);
    swap_out(&s);
}

uint32_t __wrap_lv_label_get_letter_on(const lv_obj_t *obj, lv_point_t *pos_in, bool bidi)
{
    swap_t s;
    if (!swap_in(obj, &s, NULL, 0)) return __real_lv_label_get_letter_on(obj, pos_in, bidi);
    uint32_t id = __real_lv_label_get_letter_on(obj, pos_in, bidi);
    size_t at = arabic_logical_offset(s.logical, s.len,
                                      lv_text_encoded_get_byte_id(s.shaped, id));
    swap_out(&s);
    return lv_text_encoded_get_char_id(s.logical, (uint32_t)at);
}

bool __wrap_lv_label_is_char_under_pos(const lv_obj_t *obj, lv_point_t *pos)
{
    swap_t s;
    if (!swap_in(obj, &s, NULL, 0)) return __real_lv_label_is_char_under_pos(obj, pos);
    bool under = __real_lv_label_is_char_under_pos(obj, pos);
    swap_out(&s);
    return under;
}
