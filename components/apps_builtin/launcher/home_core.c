// SPDX-License-Identifier: BSD-3-Clause
/*
 * What the Home screen decides without LVGL: see home_core.h.
 */
#include "home_core.h"

#include <stdio.h>
#include <string.h>

#define BAR_H        36   /* a 28 px button, with 4 px above and below it */
#define ROW_H        56   /* a pinned app: its 36 px icon and its name */
#define CLOCK_H_TALL 64   /* the time, and the date under it */
#define CLOCK_H_WIDE 48   /* the time, and the date beside it */
#define NOW_H_TALL   80   /* two rows of 40 */
#define NOW_H_WIDE   64   /* two rows of 30, 4 apart */
#define MAX_ROWS      2

void home_layout(int16_t width, int16_t height, home_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    out->landscape = width > height;
    out->cols = out->landscape ? 4 : 3;
    out->bar_h = BAR_H;
    out->bar_y = height > BAR_H ? (int16_t)(height - BAR_H) : 0;

    int16_t y = 0;
    int16_t room = out->bar_y;
    const int16_t clock_h = out->landscape ? CLOCK_H_WIDE : CLOCK_H_TALL;
    const int16_t now_h = out->landscape ? NOW_H_WIDE : NOW_H_TALL;

    /* The clock only with a row of apps under it, and what is new only with
     * the clock above it and a row of apps under it: on a short screen the
     * apps come first. */
    if (room >= clock_h + ROW_H) {
        out->clock_y = y;
        out->clock_h = clock_h;
        y += clock_h;
        room -= clock_h;
        if (room >= now_h + ROW_H) {
            out->now_y = y;
            out->now_h = now_h;
            y += now_h;
            room -= now_h;
        }
    }

    int rows = room / ROW_H;
    out->rows = (uint8_t)(rows > MAX_ROWS ? MAX_ROWS : rows);
    out->grid_y = y;
    out->grid_h = room;
    out->cell_h = ROW_H;
    /* The whole width, margins and all: at 240 px a cell of 80 holds
     * "Messenger", one inside the margins does not. */
    out->cell_w = (int16_t)(width > 0 ? width / out->cols : 0);
}

static bool picked(const int *out, int n, int index)
{
    for (int i = 0; i < n; i++) {
        if (out[i] == index) {
            return true;
        }
    }
    return false;
}

int home_pick_pins(const char *const *preferred, size_t n_preferred,
                   const char *const *installed, size_t n_installed,
                   bool (*eligible)(const char *id),
                   int *out, int max)
{
    int n = 0;
    for (size_t p = 0; p < n_preferred && n < max; p++) {
        for (size_t i = 0; i < n_installed; i++) {
            if (installed[i] && preferred[p] && strcmp(preferred[p], installed[i]) == 0) {
                if (!picked(out, n, (int)i)) {
                    out[n++] = (int)i;
                }
                break;
            }
        }
    }
    for (size_t i = 0; i < n_installed && n < max; i++) {
        if (!installed[i] || picked(out, n, (int)i)) {
            continue;
        }
        if (eligible && !eligible(installed[i])) {
            continue;
        }
        out[n++] = (int)i;
    }
    return n;
}

static const char *const WEEKDAYS[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

static const char *const MONTHS[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};

void home_format_date(int wday, int mday, int mon, bool two_lines,
                      char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    buf[0] = '\0';
    if (wday < 0 || wday > 6 || mon < 0 || mon > 11 || mday < 1 || mday > 31) {
        return;
    }
    snprintf(buf, len, "%s%c%d %s", WEEKDAYS[wday], two_lines ? '\n' : ' ',
             mday, MONTHS[mon]);
}
