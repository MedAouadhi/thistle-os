#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include <stdint.h>

typedef struct {
    lv_color_t primary;
    lv_color_t secondary;
    lv_color_t bg;
    lv_color_t surface;
    lv_color_t text;
    lv_color_t text_secondary;
    uint8_t radius;
    uint8_t padding;
} theme_colors_t;

/* Initialize theme engine with default theme */
esp_err_t theme_init(lv_display_t *disp);

/* Load a theme from JSON file path */
esp_err_t theme_load(const char *json_path);

/* Get current theme colors */
const theme_colors_t *theme_get_colors(void);

/* Apply current theme to an LVGL display */
esp_err_t theme_apply(lv_display_t *disp);

/* List available theme files on SD card.
 * Writes up to max_count filenames (without path) into names[].
 * Returns actual count found. */
int theme_list_available(char names[][32], int max_count);

/* Get the basename of the currently active theme (e.g. "Default" or "dark.json") */
const char *theme_get_current_name(void);

/* Gives a text field the theme's focused look, cursor and placeholder colour.
 * Every text area gets them when it is made, with a plain box beneath; call
 * this for one whose styles were taken off with lv_obj_remove_style_all(). */
void theme_style_text_field(lv_obj_t *ta);

/* Rules a 1 px line, in text_secondary, above and below each row of a list
 * made with lv_table, as the lists made of row objects have one below each
 * row. The row at the table's top edge has none above it: the title bar over
 * a list draws its own. */
void theme_rule_table_rows(lv_obj_t *table);
