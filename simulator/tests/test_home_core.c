/*
 * Unit tests for what the Home screen decides without LVGL: its bands on
 * each simulated screen, the apps it pins and the date it writes.
 *
 * The sizes are app areas, the screen less its 24 px status bar.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "test_runner.h"
#include "home_core.h"

static home_layout_t s_l;

TEST(home_tdeck_max_has_the_clock_the_new_rows_and_two_rows_of_three) {
    home_layout(240, 296, &s_l);
    ASSERT_TRUE(!s_l.landscape);
    ASSERT_EQ(s_l.clock_y, 0);
    ASSERT_EQ(s_l.clock_h, 64);
    ASSERT_EQ(s_l.now_y, 64);
    ASSERT_EQ(s_l.now_h, 80);
    ASSERT_EQ(s_l.grid_y, 144);
    ASSERT_EQ(s_l.cols, 3);
    ASSERT_EQ(s_l.rows, 2);
    ASSERT_EQ(s_l.bar_y, 260);
    ASSERT_EQ(s_l.bar_h, 36);
}

TEST(home_tdeck_lcd_has_one_row_of_four) {
    home_layout(320, 216, &s_l);
    ASSERT_TRUE(s_l.landscape);
    ASSERT_EQ(s_l.clock_h, 48);
    ASSERT_EQ(s_l.now_y, 48);
    ASSERT_EQ(s_l.now_h, 64);
    ASSERT_EQ(s_l.grid_y, 112);
    ASSERT_EQ(s_l.cols, 4);
    ASSERT_EQ(s_l.rows, 1);
    ASSERT_EQ(s_l.bar_y, 180);
}

TEST(home_tdisplay_gives_up_the_new_rows_first) {
    home_layout(320, 146, &s_l);
    ASSERT_EQ(s_l.clock_h, 48);
    ASSERT_EQ(s_l.now_h, 0);
    ASSERT_EQ(s_l.grid_y, 48);
    ASSERT_EQ(s_l.rows, 1);
}

TEST(home_cardputer_gives_up_the_clock_too) {
    home_layout(240, 111, &s_l);
    ASSERT_EQ(s_l.clock_h, 0);
    ASSERT_EQ(s_l.now_h, 0);
    ASSERT_EQ(s_l.grid_y, 0);
    ASSERT_EQ(s_l.rows, 1);
    ASSERT_EQ(s_l.cols, 4);
}

TEST(home_an_oled_has_room_for_the_bar_alone) {
    home_layout(128, 40, &s_l);
    ASSERT_EQ(s_l.clock_h, 0);
    ASSERT_EQ(s_l.rows, 0);
    ASSERT_EQ(s_l.bar_y, 4);
}

TEST(home_a_tall_screen_stops_at_two_rows) {
    home_layout(410, 478, &s_l);
    ASSERT_EQ(s_l.cols, 3);
    ASSERT_EQ(s_l.rows, 2);
    ASSERT_EQ(s_l.bar_y, 442);
}

TEST(home_cells_share_the_whole_width) {
    /* 80 px holds "Messenger", 78 px in Montserrat 14. */
    home_layout(240, 296, &s_l);
    ASSERT_EQ(s_l.cell_w, 80);
    ASSERT_EQ(s_l.cell_h, 56);
    home_layout(320, 216, &s_l);
    ASSERT_EQ(s_l.cell_w, 80);
}

static const char *const PREFERRED[] = {
    "com.thistle.phone", "com.thistle.messenger", "com.thistle.hermes",
};

static bool not_settings(const char *id)
{
    return strcmp(id, "com.thistle.settings") != 0;
}

TEST(home_pins_the_preferred_apps_in_their_order) {
    const char *installed[] = {
        "com.thistle.settings", "com.thistle.hermes", "com.thistle.phone",
        "com.thistle.messenger",
    };
    int out[HOME_MAX_PINS];
    ASSERT_EQ(home_pick_pins(PREFERRED, 3, installed, 4, not_settings, out, 3), 3);
    ASSERT_EQ(out[0], 2);
    ASSERT_EQ(out[1], 3);
    ASSERT_EQ(out[2], 1);
}

TEST(home_fills_the_place_of_a_missing_app_from_the_rest) {
    /* A board with no radio has no Messenger. */
    const char *installed[] = {
        "com.thistle.settings", "com.thistle.notes", "com.thistle.phone",
        "com.thistle.hermes", "com.thistle.reader",
    };
    int out[HOME_MAX_PINS];
    ASSERT_EQ(home_pick_pins(PREFERRED, 3, installed, 5, not_settings, out, 4), 4);
    ASSERT_EQ(out[0], 2);
    ASSERT_EQ(out[1], 3);
    ASSERT_EQ(out[2], 1);  /* notes, not settings */
    ASSERT_EQ(out[3], 4);
}

TEST(home_pins_no_more_than_asked_for) {
    const char *installed[] = {
        "com.thistle.phone", "com.thistle.messenger", "com.thistle.hermes",
    };
    int out[HOME_MAX_PINS];
    ASSERT_EQ(home_pick_pins(PREFERRED, 3, installed, 3, NULL, out, 2), 2);
    ASSERT_EQ(out[0], 0);
    ASSERT_EQ(out[1], 1);
    ASSERT_EQ(home_pick_pins(PREFERRED, 3, installed, 3, NULL, out, 0), 0);
}

TEST(home_pins_an_app_once_though_preferred_twice) {
    const char *const twice[] = { "com.thistle.phone", "com.thistle.phone" };
    const char *installed[] = { "com.thistle.phone", "com.thistle.notes" };
    int out[HOME_MAX_PINS];
    ASSERT_EQ(home_pick_pins(twice, 2, installed, 2, NULL, out, 4), 2);
    ASSERT_EQ(out[0], 0);
    ASSERT_EQ(out[1], 1);
}

TEST(home_writes_the_date_on_one_line) {
    char buf[40];
    home_format_date(5, 16, 9, false, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "Friday 16 October");
}

TEST(home_writes_the_weekday_on_its_own_line_when_asked) {
    char buf[40];
    home_format_date(3, 2, 8, true, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "Wednesday\n2 September");
}

TEST(home_writes_nothing_for_a_date_out_of_range) {
    char buf[40] = "stale";
    home_format_date(7, 16, 9, false, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "");
    strcpy(buf, "stale");
    home_format_date(5, 16, 12, false, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "");
}

TEST(home_cuts_the_date_to_the_buffer) {
    char buf[8];
    home_format_date(5, 16, 9, false, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "Friday ");
}
