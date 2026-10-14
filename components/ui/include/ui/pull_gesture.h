// SPDX-License-Identifier: BSD-3-Clause
#pragma once

/*
 * The drag that opens the quick panel, and the one that closes it, picked
 * out of the raw touch stream (pull_gesture.c).
 *
 * A touch that goes down in the band at the top of the screen (the status
 * bar) and moves down far enough, more down than sideways, opens the panel.
 * While the panel is open, a touch anywhere that moves up as far, more up
 * than sideways, closes it; a sideways drag, along a slider, does not. Each
 * touch acts once, as soon as it has gone far enough: the finger need not
 * lift.
 *
 * Nothing here knows LVGL or the time, so it is tested on the host
 * (simulator/tests/test_pull_gesture.c).
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PULL_NONE = 0,
    PULL_OPEN,
    PULL_CLOSE,
} pull_action_t;

typedef enum {
    PULL_TOUCH_DOWN,
    PULL_TOUCH_MOVE,
    PULL_TOUCH_UP,
} pull_touch_t;

typedef struct {
    int16_t band_h;     /* a pull down starts above this y */
    int16_t threshold;  /* how far a pull has to go, in pixels */
    bool    tracking;   /* a finger is down */
    bool    from_band;  /* it went down in the band */
    bool    done;       /* this touch has acted */
    int16_t x0, y0;     /* where it went down */
} pull_gesture_t;

/* The shortest pull, on a small screen. */
#define PULL_MIN_THRESHOLD 32

/* A pull goes an eighth of the screen's height, and at least
 * PULL_MIN_THRESHOLD. */
void pull_gesture_init(pull_gesture_t *g, int16_t band_h, int16_t screen_h);

/* Feed one touch event; `panel_open` is whether the panel is open now. What
 * the touch has done, if it has just done it. */
pull_action_t pull_gesture_feed(pull_gesture_t *g, pull_touch_t kind,
                                int16_t x, int16_t y, bool panel_open);
