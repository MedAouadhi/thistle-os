/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the drags that open and close the quick panel
 */

#include "ui/pull_gesture.h"

#include <stdlib.h>

void pull_gesture_init(pull_gesture_t *g, int16_t band_h, int16_t screen_h)
{
    int16_t threshold = (int16_t)(screen_h / 8);
    if (threshold < PULL_MIN_THRESHOLD) {
        threshold = PULL_MIN_THRESHOLD;
    }
    *g = (pull_gesture_t){
        .band_h    = band_h,
        .threshold = threshold,
    };
}

static void start(pull_gesture_t *g, int16_t x, int16_t y)
{
    g->tracking  = true;
    g->done      = false;
    g->from_band = y < g->band_h;
    g->x0        = x;
    g->y0        = y;
}

pull_action_t pull_gesture_feed(pull_gesture_t *g, pull_touch_t kind,
                                int16_t x, int16_t y, bool panel_open)
{
    if (kind == PULL_TOUCH_DOWN) {
        start(g, x, y);
        return PULL_NONE;
    }
    if (!g->tracking) {
        /* A move with no press before it: a driver that reports the first
         * point as a move. It starts here. */
        if (kind == PULL_TOUCH_MOVE) {
            start(g, x, y);
        }
        return PULL_NONE;
    }
    if (kind == PULL_TOUCH_UP) {
        g->tracking = false;
    }
    if (g->done) {
        return PULL_NONE;
    }

    int dx = abs(x - g->x0);
    int dy = y - g->y0;

    if (!panel_open && g->from_band && dy >= g->threshold && dy > dx) {
        g->done = true;
        return PULL_OPEN;
    }
    if (panel_open && -dy >= g->threshold && -dy > dx) {
        g->done = true;
        return PULL_CLOSE;
    }
    return PULL_NONE;
}
