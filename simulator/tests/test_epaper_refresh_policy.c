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
