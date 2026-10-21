/*
 * Unit tests for the e-paper refresh policy: which panel refreshes are full
 * and which are fast, under each mode Settings offers.
 *
 * The panel driver does what it is told on each refresh; this is where the
 * user's choice turns into those instructions. Before it, Settings set the
 * driver's mode directly and the driver reverted Full to Fast after one
 * refresh, so none of the three buttons changed anything for long.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "test_runner.h"
#include "ui/epaper_refresh.h"

#define DISP_W 240
#define DISP_H 320

TEST(epaper_policy_defaults_to_fast) {
    /* What the panel did before there was a choice: fast after the first
     * paint, which the driver makes full on its own. */
    ASSERT_EQ(epaper_refresh_init(DISP_W, DISP_H), ESP_OK);
    ASSERT_EQ(epaper_refresh_get_policy(), EPAPER_REFRESH_POLICY_FAST);
    for (int i = 0; i < 20; i++) {
        ASSERT_TRUE(!epaper_refresh_next_is_full());
    }
}

TEST(epaper_policy_full_stays_full) {
    ASSERT_EQ(epaper_refresh_init(DISP_W, DISP_H), ESP_OK);
    epaper_refresh_set_policy(EPAPER_REFRESH_POLICY_FULL);
    for (int i = 0; i < 20; i++) {
        ASSERT_TRUE(epaper_refresh_next_is_full());
    }
}

TEST(epaper_policy_auto_cleans_after_every_run_of_fast_refreshes) {
    ASSERT_EQ(epaper_refresh_init(DISP_W, DISP_H), ESP_OK);
    epaper_refresh_set_policy(EPAPER_REFRESH_POLICY_AUTO);
    for (int cycle = 0; cycle < 3; cycle++) {
        for (int i = 0; i < EPAPER_REFRESH_AUTO_FAST_RUN; i++) {
            ASSERT_TRUE(!epaper_refresh_next_is_full());
        }
        ASSERT_TRUE(epaper_refresh_next_is_full());
    }
}

TEST(epaper_policy_requested_full_happens_once_under_fast) {
    ASSERT_EQ(epaper_refresh_init(DISP_W, DISP_H), ESP_OK);
    epaper_refresh_request_full();
    ASSERT_TRUE(epaper_refresh_next_is_full());
    ASSERT_TRUE(!epaper_refresh_next_is_full());
    ASSERT_TRUE(!epaper_refresh_next_is_full());
}

TEST(epaper_policy_requested_full_restarts_the_auto_run) {
    /* A full refresh clears the ghosting whoever asked for it, so Auto counts
     * its run of fast refreshes from there. */
    ASSERT_EQ(epaper_refresh_init(DISP_W, DISP_H), ESP_OK);
    epaper_refresh_set_policy(EPAPER_REFRESH_POLICY_AUTO);
    ASSERT_TRUE(!epaper_refresh_next_is_full());
    ASSERT_TRUE(!epaper_refresh_next_is_full());
    epaper_refresh_request_full();
    ASSERT_TRUE(epaper_refresh_next_is_full());
    for (int i = 0; i < EPAPER_REFRESH_AUTO_FAST_RUN; i++) {
        ASSERT_TRUE(!epaper_refresh_next_is_full());
    }
    ASSERT_TRUE(epaper_refresh_next_is_full());
}

TEST(epaper_policy_unknown_value_falls_back_to_fast) {
    ASSERT_EQ(epaper_refresh_init(DISP_W, DISP_H), ESP_OK);
    epaper_refresh_set_policy(EPAPER_REFRESH_POLICY_FULL);
    epaper_refresh_set_policy((epaper_refresh_policy_t)7);
    ASSERT_EQ(epaper_refresh_get_policy(), EPAPER_REFRESH_POLICY_FAST);
    ASSERT_TRUE(!epaper_refresh_next_is_full());
}

TEST(epaper_policy_init_forgets_the_policy_and_any_request) {
    ASSERT_EQ(epaper_refresh_init(DISP_W, DISP_H), ESP_OK);
    epaper_refresh_set_policy(EPAPER_REFRESH_POLICY_FULL);
    epaper_refresh_request_full();
    ASSERT_EQ(epaper_refresh_init(DISP_W, DISP_H), ESP_OK);
    ASSERT_EQ(epaper_refresh_get_policy(), EPAPER_REFRESH_POLICY_FAST);
    ASSERT_TRUE(!epaper_refresh_next_is_full());
}

/* When the panel refreshes after LVGL draws. It used to wait 300 ms after the
 * last flush, a third of the time from a key press to its result on a T-Deck
 * Max; now it waits for the screen to settle, and no longer than a second for
 * a screen that keeps moving with no finger on it. */

#define MS 1000
#define NO_TOUCH INT64_MIN / 2   /* no finger on the screen for ages */

TEST(epaper_refresh_waits_for_the_screen_to_settle) {
    /* One pass of drawing, at 0 ms: not yet, as LVGL may draw the rest of the
     * change on its next pass, 33 ms on. */
    ASSERT_TRUE(!epaper_refresh_settled(0, 0, 0, NO_TOUCH));
    ASSERT_TRUE(!epaper_refresh_settled(40 * MS, 0, 0, NO_TOUCH));
    ASSERT_TRUE(epaper_refresh_settled(EPAPER_REFRESH_SETTLE_US, 0, 0, NO_TOUCH));
}

TEST(epaper_refresh_settles_from_the_last_flush) {
    /* Drawn at 0 and again at 33 ms: settled 60 ms after the second. */
    ASSERT_TRUE(!epaper_refresh_settled(80 * MS, 0, 33 * MS, NO_TOUCH));
    ASSERT_TRUE(epaper_refresh_settled(33 * MS + EPAPER_REFRESH_SETTLE_US, 0, 33 * MS, NO_TOUCH));
}

TEST(epaper_refresh_shows_a_screen_that_never_settles) {
    /* Something animating draws every 33 ms. It is shown once a second has
     * passed since its first flush, however recent the last. */
    int64_t first = 5000 * MS;
    int64_t now = first;
    while (!epaper_refresh_settled(now, first, now, NO_TOUCH)) {
        now += 33 * MS;
        ASSERT_TRUE(now - first <= EPAPER_REFRESH_MAX_WAIT_US + 33 * MS);
    }
    ASSERT_TRUE(now - first >= EPAPER_REFRESH_MAX_WAIT_US);
}

TEST(epaper_refresh_does_not_show_a_press_about_to_be_let_go) {
    /* A finger down on an app icon, or Enter on a focused button, draws it
     * pressed, black on e-paper, and lets go a moment later. Shown at once,
     * every tap, and every drag that began on an icon, flashed it black for
     * a whole refresh. The press is shown once it is a long press. */
    int64_t now = 5000 * MS;
    ASSERT_TRUE(!epaper_refresh_settled(now, now - 100 * MS, now - 100 * MS, now));
    ASSERT_TRUE(epaper_refresh_settled(now, now - EPAPER_REFRESH_HELD_US,
                                       now - EPAPER_REFRESH_HELD_US, now));
}

TEST(epaper_refresh_waits_for_lvgl_to_draw_a_let_go) {
    /* LVGL reads the release up to a pass later and draws it a pass after
     * that, so the pressed frame is not shown in between. */
    int64_t now = 5000 * MS;
    ASSERT_TRUE(!epaper_refresh_settled(now, now - 200 * MS, now - 200 * MS, now - 10 * MS));
    ASSERT_TRUE(epaper_refresh_settled(now, now - 200 * MS, now - EPAPER_REFRESH_SETTLE_US,
                                       now - EPAPER_REFRESH_LET_GO_US));
}

TEST(epaper_refresh_waits_for_a_dragging_finger) {
    /* A list under a finger draws every 33 ms, but refreshing the panel
     * mid-drag only kept the frame it stopped on waiting for that refresh.
     * It is shown when the finger lifts, or pauses as long as a long press. */
    int64_t first = 5000 * MS;
    int64_t now = first + 3000 * MS;
    ASSERT_TRUE(!epaper_refresh_settled(now, first, now - 10 * MS, now));
    ASSERT_TRUE(!epaper_refresh_settled(now, first, now - 10 * MS, now - 10 * MS));
    ASSERT_TRUE(epaper_refresh_settled(now, first, now - 10 * MS,
                                       now - EPAPER_REFRESH_LET_GO_US));
    ASSERT_TRUE(epaper_refresh_settled(now, first, now - EPAPER_REFRESH_HELD_US, now));
}
