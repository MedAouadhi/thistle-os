/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Hermes's messages drawn in LVGL.
 *
 * A message is Markdown (hermes_markdown.h): each block becomes a label in
 * a shared style, a table a grid of labels if it fits the width or a run of
 * "Header: value" groups if not (hermes_esp32 ADR 0050). Inline emphasis is
 * not drawn: the build has no bold face.
 *
 * LVGL copies a label's text into its 64 KB pool, which a transcript would
 * fill. So every text is written once, in characters the fonts can draw
 * (hermes_glyphs.h), into an arena of the item's own in PSRAM, and the
 * labels point at it (lv_label_set_text_static). The arena must outlive
 * them: free it after deleting the objects.
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *buf;
    size_t cap;
    size_t used;
} hermes_arena_t;

bool hermes_arena_init(hermes_arena_t *arena, size_t cap);
void hermes_arena_free(hermes_arena_t *arena);

/* `len` bytes of `text`, made drawable, kept in the arena; NULL when it is
 * full. */
const char *hermes_arena_put(hermes_arena_t *arena, const char *text, size_t len);

/* The styles every view shares. Made once, when the app opens, and reset
 * when it closes. */
typedef struct {
    lv_style_t body;
    lv_style_t secondary;
    lv_style_t heading;
    lv_style_t subheading;
    lv_style_t code;
    lv_style_t quote;
    lv_style_t rule;
    lv_style_t cell_head;
    lv_style_t row;
    lv_style_t box;
    lv_style_t user;          /* the box around the user's own message */
    lv_style_t page;          /* the app's ground */
    lv_style_t bar;           /* the header and the compose bar */
    lv_style_t button;
    lv_style_t button_pressed;
    lv_style_t button_inert;  /* a row that cannot be chosen */
    lv_style_t focus;
    lv_style_t field;         /* a text field's frame */
} hermes_view_styles_t;

void hermes_view_styles_init(void);
void hermes_view_styles_reset(void);
const hermes_view_styles_t *hermes_view_styles(void);

/* A label for `text` in `style`, its text kept in the arena. */
lv_obj_t *hermes_view_label(lv_obj_t *parent, hermes_arena_t *arena, const char *text,
                            size_t len, const lv_style_t *style);

/* Draws `markdown` into `parent`, a column `width` pixels wide. */
void hermes_view_markdown(lv_obj_t *parent, hermes_arena_t *arena, const char *markdown,
                          int32_t width);
