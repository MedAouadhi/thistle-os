/*
 * Unit tests for the drags that open and close the quick panel.
 *
 * The screen is the T-Deck Max's, 240 x 320 with a 24 px status bar, so a
 * pull goes 40 px.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "test_runner.h"
#include "ui/pull_gesture.h"

#define BAR_H    24
#define SCREEN_H 320

static pull_gesture_t s_g;

/* A drag from (x0, y0) to (x1, y1) in `steps` moves, then a lift. Whatever
 * it did first, or PULL_NONE. */
static pull_action_t drag(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                          int steps, bool panel_open)
{
    pull_action_t first = pull_gesture_feed(&s_g, PULL_TOUCH_DOWN, x0, y0, panel_open);
    for (int i = 1; i <= steps; i++) {
        int16_t x = (int16_t)(x0 + (x1 - x0) * i / steps);
        int16_t y = (int16_t)(y0 + (y1 - y0) * i / steps);
        pull_action_t a = pull_gesture_feed(&s_g, PULL_TOUCH_MOVE, x, y, panel_open);
        if (first == PULL_NONE) first = a;
    }
    pull_action_t a = pull_gesture_feed(&s_g, PULL_TOUCH_UP, x1, y1, panel_open);
    if (first == PULL_NONE) first = a;
    return first;
}

TEST(pull_threshold_is_an_eighth_of_the_screen) {
    pull_gesture_init(&s_g, BAR_H, SCREEN_H);
    ASSERT_EQ(s_g.threshold, 40);
}

TEST(pull_threshold_has_a_floor_on_a_short_screen) {
    pull_gesture_init(&s_g, BAR_H, 135);
    ASSERT_EQ(s_g.threshold, PULL_MIN_THRESHOLD);
}

TEST(pull_down_from_the_bar_opens) {
    pull_gesture_init(&s_g, BAR_H, SCREEN_H);
    ASSERT_EQ(drag(120, 10, 125, 120, 6, false), PULL_OPEN);
}

TEST(pull_down_opens_before_the_finger_lifts) {
    pull_gesture_init(&s_g, BAR_H, SCREEN_H);
    ASSERT_EQ(pull_gesture_feed(&s_g, PULL_TOUCH_DOWN, 100, 5, false), PULL_NONE);
    ASSERT_EQ(pull_gesture_feed(&s_g, PULL_TOUCH_MOVE, 100, 30, false), PULL_NONE);
    ASSERT_EQ(pull_gesture_feed(&s_g, PULL_TOUCH_MOVE, 100, 45, false), PULL_OPEN);
}

TEST(pull_down_from_below_the_bar_does_nothing) {
    /* A list being scrolled in the app. */
    pull_gesture_init(&s_g, BAR_H, SCREEN_H);
    ASSERT_EQ(drag(120, 40, 120, 200, 6, false), PULL_NONE);
}

TEST(pull_too_short_does_nothing) {
    pull_gesture_init(&s_g, BAR_H, SCREEN_H);
    ASSERT_EQ(drag(120, 10, 120, 45, 3, false), PULL_NONE);
}

TEST(pull_more_sideways_than_down_does_nothing) {
    pull_gesture_init(&s_g, BAR_H, SCREEN_H);
    ASSERT_EQ(drag(20, 10, 200, 60, 6, false), PULL_NONE);
}

TEST(pull_a_tap_on_the_bar_does_nothing) {
    /* The unread-message count in the bar takes taps. */
    pull_gesture_init(&s_g, BAR_H, SCREEN_H);
    ASSERT_EQ(drag(200, 12, 200, 12, 1, false), PULL_NONE);
}

TEST(pull_down_acts_once_per_touch) {
    pull_gesture_init(&s_g, BAR_H, SCREEN_H);
    ASSERT_EQ(pull_gesture_feed(&s_g, PULL_TOUCH_DOWN, 100, 5, false), PULL_NONE);
    ASSERT_EQ(pull_gesture_feed(&s_g, PULL_TOUCH_MOVE, 100, 60, false), PULL_OPEN);
    /* The panel is open now, and the same finger keeps going. */
    ASSERT_EQ(pull_gesture_feed(&s_g, PULL_TOUCH_MOVE, 100, 200, true), PULL_NONE);
    ASSERT_EQ(pull_gesture_feed(&s_g, PULL_TOUCH_MOVE, 100, 10, true), PULL_NONE);
    ASSERT_EQ(pull_gesture_feed(&s_g, PULL_TOUCH_UP, 100, 10, true), PULL_NONE);
}

TEST(pull_down_does_not_open_an_open_panel_again) {
    pull_gesture_init(&s_g, BAR_H, SCREEN_H);
    ASSERT_EQ(drag(120, 10, 120, 150, 6, true), PULL_NONE);
}

TEST(pull_up_closes_an_open_panel) {
    pull_gesture_init(&s_g, BAR_H, SCREEN_H);
    ASSERT_EQ(drag(120, 200, 118, 60, 6, true), PULL_CLOSE);
}

TEST(pull_up_does_nothing_with_the_panel_shut) {
    pull_gesture_init(&s_g, BAR_H, SCREEN_H);
    ASSERT_EQ(drag(120, 200, 120, 30, 6, false), PULL_NONE);
}

TEST(pull_a_slider_drag_does_not_close) {
    /* Along the slider, drifting up a little as a thumb does. */
    pull_gesture_init(&s_g, BAR_H, SCREEN_H);
    ASSERT_EQ(drag(30, 180, 210, 150, 8, true), PULL_NONE);
}

TEST(pull_a_move_with_no_press_starts_a_touch) {
    /* A driver that reports the first point as a move. */
    pull_gesture_init(&s_g, BAR_H, SCREEN_H);
    ASSERT_EQ(pull_gesture_feed(&s_g, PULL_TOUCH_MOVE, 100, 8, false), PULL_NONE);
    ASSERT_EQ(pull_gesture_feed(&s_g, PULL_TOUCH_MOVE, 100, 70, false), PULL_OPEN);
}

TEST(pull_a_lift_with_no_press_does_nothing) {
    pull_gesture_init(&s_g, BAR_H, SCREEN_H);
    ASSERT_EQ(pull_gesture_feed(&s_g, PULL_TOUCH_UP, 100, 300, false), PULL_NONE);
    ASSERT_EQ(pull_gesture_feed(&s_g, PULL_TOUCH_UP, 100, 300, true), PULL_NONE);
}

TEST(pull_each_touch_starts_afresh) {
    pull_gesture_init(&s_g, BAR_H, SCREEN_H);
    ASSERT_EQ(drag(120, 10, 120, 150, 4, false), PULL_OPEN);
    ASSERT_EQ(drag(120, 250, 120, 100, 4, true), PULL_CLOSE);
    ASSERT_EQ(drag(120, 10, 120, 150, 4, false), PULL_OPEN);
}
