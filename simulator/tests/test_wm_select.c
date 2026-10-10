/*
 * Unit tests for window manager selection.
 *
 * The pairing of manager and hosts_lvgl_apps is the part worth pinning: get
 * it wrong in the permissive direction and the LVGL built-ins register under
 * a manager that cannot host them, which segfaults on launch rather than
 * failing politely.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "test_runner.h"
#include "ui/lvgl_wm.h"

#include <string.h>

TEST(wm_select_defaults_an_epaper_board_to_the_lvgl_epaper_manager) {
    /* thistle-tk hosts three of the fourteen built-ins, so defaulting to it
     * left the T-Deck Pro and Max without the rest. */
    thistle_wm_choice_t choice = thistle_wm_select(NULL, true);
    ASSERT_TRUE(choice.wm == lvgl_epaper_wm_get());
    ASSERT_TRUE(choice.hosts_lvgl_apps);
}

TEST(wm_select_defaults_an_lcd_board_to_the_lvgl_lcd_manager) {
    thistle_wm_choice_t choice = thistle_wm_select(NULL, false);
    ASSERT_TRUE(choice.wm == lvgl_lcd_wm_get());
    ASSERT_TRUE(choice.hosts_lvgl_apps);
}

TEST(wm_select_treats_empty_and_auto_as_no_preference) {
    ASSERT_TRUE(thistle_wm_select("", true).wm == lvgl_epaper_wm_get());
    ASSERT_TRUE(thistle_wm_select("auto", true).wm == lvgl_epaper_wm_get());
}

TEST(wm_select_honours_the_lvgl_epaper_manager_when_asked_for_it) {
    thistle_wm_choice_t choice = thistle_wm_select("lvgl-epaper-wm", true);
    ASSERT_TRUE(choice.wm == lvgl_epaper_wm_get());
    ASSERT_TRUE(choice.hosts_lvgl_apps);
}

TEST(wm_select_honours_thistle_tk_on_an_epaper_board) {
    /* The opt-out from the default: cheaper refreshes, fewer apps. */
    thistle_wm_choice_t choice = thistle_wm_select("thistle-tk", true);
    ASSERT_TRUE(choice.wm == thistle_tk_wm_get());
    ASSERT_TRUE(!choice.hosts_lvgl_apps);
}

TEST(wm_select_honours_thistle_tk_on_a_board_that_would_default_otherwise) {
    thistle_wm_choice_t choice = thistle_wm_select("thistle-tk", false);
    ASSERT_TRUE(choice.wm == thistle_tk_wm_get());
    ASSERT_TRUE(!choice.hosts_lvgl_apps);
}

TEST(wm_select_falls_back_when_the_name_matches_no_manager) {
    /* "lvgl-wm" is what system.json shipped for as long as the value was
     * ignored, and it names none of the three. A board carrying the old value
     * gets its display's default rather than landing somewhere arbitrary. */
    thistle_wm_choice_t choice = thistle_wm_select("lvgl-wm", true);
    ASSERT_TRUE(choice.wm == lvgl_epaper_wm_get());
    ASSERT_TRUE(choice.hosts_lvgl_apps);

    choice = thistle_wm_select("lvgl-wm", false);
    ASSERT_TRUE(choice.wm == lvgl_lcd_wm_get());
    ASSERT_TRUE(choice.hosts_lvgl_apps);
}

TEST(wm_select_never_offers_lvgl_apps_to_thistle_tk) {
    /* Whichever way thistle-tk is arrived at, it must not be handed the apps
     * that parent to an LVGL app area it does not create. */
    const char *routes[] = { NULL, "", "auto", "thistle-tk", "nonsense" };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        thistle_wm_choice_t choice = thistle_wm_select(routes[i], true);
        if (choice.wm == thistle_tk_wm_get()) {
            ASSERT_TRUE(!choice.hosts_lvgl_apps);
        }
    }
}
