#pragma once

/*
 * Arabic in every label and text field: drawn in a font that has it, with its
 * letters joined and read right to left, whatever font the widget was given.
 * See arabic_text.c.
 */

#include "lvgl.h"

/* Picks the Arabic fonts for the display, one bit a pixel on e-paper, and has
 * every text on its screen and layers find its own direction. Called once,
 * when the UI starts; until then the antialiased fonts are used. */
void arabic_text_init(lv_display_t *disp);
