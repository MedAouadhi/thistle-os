// SPDX-License-Identifier: BSD-3-Clause
#pragma once

/*
 * What the Home screen decides without LVGL (home_core.c): where each band
 * goes on a screen of a given size, which apps are pinned, and the date as
 * Home writes it. launcher_ui.c draws what these say.
 *
 * Nothing here knows LVGL, the kernel or the time, so it is tested on the
 * host (simulator/tests/test_home_core.c).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The most apps Home pins: four columns of two rows. */
#define HOME_MAX_PINS 8

/* Left and right of the All apps button. */
#define HOME_MARGIN 8

/* Home, top to bottom, in the app area below the status bar. A band with no
 * room on the screen has a height of 0; the bar always has its place. */
typedef struct {
    bool    landscape;        /* wider than tall: the date goes beside the time */
    int16_t clock_y, clock_h;
    int16_t now_y, now_h;     /* what is new: room kept, nothing drawn yet */
    int16_t grid_y, grid_h;   /* the pinned apps, `rows` of `cols` */
    uint8_t cols, rows;
    int16_t cell_w, cell_h;
    int16_t bar_y, bar_h;     /* the All apps button */
} home_layout_t;

/* The bands for an app area `width` by `height`. */
void home_layout(int16_t width, int16_t height, home_layout_t *out);

/* The apps to pin, at most `max`: those of `preferred` that are installed,
 * in that order, then the other installed apps `eligible` accepts, in the
 * order `installed` lists them. Writes indexes into `installed` to `out` and
 * returns how many. `eligible` may be NULL, to accept every app. */
int home_pick_pins(const char *const *preferred, size_t n_preferred,
                   const char *const *installed, size_t n_installed,
                   bool (*eligible)(const char *id),
                   int *out, int max);

/* The date as Home shows it, "Friday 16 October", or with the weekday on a
 * line of its own when `two_lines`. `wday` 0 is Sunday and `mon` 0 January,
 * as in struct tm; anything out of range writes "". */
void home_format_date(int wday, int mday, int mon, bool two_lines,
                      char *buf, size_t len);
