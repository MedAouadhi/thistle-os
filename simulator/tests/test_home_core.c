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
    home_layout(240, 296, true, &s_l);
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
    home_layout(320, 216, true, &s_l);
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
    home_layout(320, 146, true, &s_l);
    ASSERT_EQ(s_l.clock_h, 48);
    ASSERT_EQ(s_l.now_h, 0);
    ASSERT_EQ(s_l.grid_y, 48);
    ASSERT_EQ(s_l.rows, 1);
}

TEST(home_cardputer_gives_up_the_clock_too) {
    home_layout(240, 111, true, &s_l);
    ASSERT_EQ(s_l.clock_h, 0);
    ASSERT_EQ(s_l.now_h, 0);
    ASSERT_EQ(s_l.grid_y, 0);
    ASSERT_EQ(s_l.rows, 1);
    ASSERT_EQ(s_l.cols, 4);
}

TEST(home_an_oled_has_room_for_the_bar_alone) {
    home_layout(128, 40, true, &s_l);
    ASSERT_EQ(s_l.clock_h, 0);
    ASSERT_EQ(s_l.rows, 0);
    ASSERT_EQ(s_l.bar_y, 4);
}

TEST(home_a_tall_screen_stops_at_two_rows) {
    home_layout(410, 478, true, &s_l);
    ASSERT_EQ(s_l.cols, 3);
    ASSERT_EQ(s_l.rows, 2);
    ASSERT_EQ(s_l.bar_y, 442);
}

TEST(home_tdeck_max_with_its_apps_key_gives_the_bar_to_the_apps) {
    /* The rows of apps sit in the middle of what is left: 20 px from what
     * is new, where the bar left them 2. */
    home_layout(240, 296, false, &s_l);
    ASSERT_EQ(s_l.grid_y, 144);
    ASSERT_EQ(s_l.grid_h, 152);
    ASSERT_EQ(s_l.rows, 2);
    ASSERT_EQ(s_l.bar_h, 0);
    ASSERT_EQ(s_l.bar_y, 296);
}

TEST(home_cells_share_the_whole_width) {
    /* 80 px holds a name as long as "Messenger", 78 px in Montserrat 14. */
    home_layout(240, 296, true, &s_l);
    ASSERT_EQ(s_l.cell_w, 80);
    ASSERT_EQ(s_l.cell_h, 56);
    home_layout(320, 216, true, &s_l);
    ASSERT_EQ(s_l.cell_w, 80);
}

static const char *const PREFERRED[] = {
    "com.thistle.phone", "com.thistle.sms", "com.thistle.hermes",
};

static bool not_settings(const char *id)
{
    return strcmp(id, "com.thistle.settings") != 0;
}

TEST(home_pins_the_preferred_apps_in_their_order) {
    const char *installed[] = {
        "com.thistle.settings", "com.thistle.hermes", "com.thistle.phone",
        "com.thistle.sms",
    };
    int out[HOME_MAX_PINS];
    ASSERT_EQ(home_pick_pins(PREFERRED, 3, installed, 4, not_settings, out, 3), 3);
    ASSERT_EQ(out[0], 2);
    ASSERT_EQ(out[1], 3);
    ASSERT_EQ(out[2], 1);
}

TEST(home_fills_the_place_of_a_missing_app_from_the_rest) {
    /* A board with no SMS app. */
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
        "com.thistle.phone", "com.thistle.sms", "com.thistle.hermes",
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

static const char *const INSTALLED6[] = {
    "com.thistle.phone", "com.thistle.sms", "com.thistle.hermes",
    "com.thistle.notes", "com.thistle.reader", "com.thistle.todo",
};

TEST(home_places_with_no_choice_are_the_default_pins) {
    int out[HOME_MAX_PINS];
    ASSERT_EQ(home_place_pins(NULL, 0, PREFERRED, 3, INSTALLED6, 6, NULL, out, 4), 4);
    ASSERT_EQ(out[0], 0);
    ASSERT_EQ(out[1], 1);
    ASSERT_EQ(out[2], 2);
    ASSERT_EQ(out[3], 3);
}

TEST(home_places_keep_the_users_choice_where_they_made_it) {
    const char *chosen[] = { "com.thistle.todo", "", "com.thistle.phone" };
    int out[HOME_MAX_PINS];
    ASSERT_EQ(home_place_pins(chosen, 3, PREFERRED, 3, INSTALLED6, 6, NULL, out, 4), 4);
    ASSERT_EQ(out[0], 5);  /* todo, chosen */
    ASSERT_EQ(out[1], 1);  /* sms: phone is placed already */
    ASSERT_EQ(out[2], 0);  /* phone, chosen */
    ASSERT_EQ(out[3], 2);  /* hermes */
}

TEST(home_places_fill_a_choice_no_longer_installed) {
    const char *chosen[] = { "com.thistle.gone", "com.thistle.reader" };
    int out[HOME_MAX_PINS];
    ASSERT_EQ(home_place_pins(chosen, 2, PREFERRED, 3, INSTALLED6, 6, NULL, out, 3), 3);
    ASSERT_EQ(out[0], 0);  /* phone, the first default */
    ASSERT_EQ(out[1], 4);  /* reader, chosen */
    ASSERT_EQ(out[2], 1);
}

TEST(home_places_an_app_chosen_twice_once) {
    const char *chosen[] = { "com.thistle.sms", "com.thistle.sms" };
    int out[HOME_MAX_PINS];
    ASSERT_EQ(home_place_pins(chosen, 2, PREFERRED, 3, INSTALLED6, 6, NULL, out, 2), 2);
    ASSERT_EQ(out[0], 1);
    ASSERT_EQ(out[1], 0);
}

TEST(home_places_left_empty_when_there_are_too_few_apps) {
    const char *installed[] = { "com.thistle.phone", "com.thistle.sms" };
    const char *chosen[] = { "", "", "com.thistle.sms" };
    int out[HOME_MAX_PINS];
    ASSERT_EQ(home_place_pins(chosen, 3, PREFERRED, 3, installed, 2, NULL, out, 6), 3);
    ASSERT_EQ(out[0], 0);
    ASSERT_EQ(out[1], -1);
    ASSERT_EQ(out[2], 1);
}

TEST(home_choosing_a_pinned_app_swaps_the_two_places) {
    char ids[3][HOME_PIN_ID_MAX] = { "a", "b", "c" };
    home_choose_pin(ids, 3, 0, "c");
    ASSERT_STR_EQ(ids[0], "c");
    ASSERT_STR_EQ(ids[1], "b");
    ASSERT_STR_EQ(ids[2], "a");
    home_choose_pin(ids, 3, 1, "z");
    ASSERT_STR_EQ(ids[1], "z");
    home_choose_pin(ids, 3, 3, "y");   /* no such place */
    ASSERT_STR_EQ(ids[2], "a");
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

/* ------------------------------------------------------------------ */
/* What is new                                                          */
/* ------------------------------------------------------------------ */

#define DOT " \xE2\x80\xA2 "

static home_now_row_t s_rows[HOME_NOW_ROWS];

/* Nothing new, the clock unset, the tailnet quiet, the mesh off. */
static home_now_in_t quiet(void)
{
    home_now_in_t in;
    memset(&in, 0, sizeof(in));
    in.missed_hour = -1;
    in.format_24h = true;
    return in;
}

TEST(home_now_with_nothing_new_is_all_caught_up_alone) {
    home_now_in_t in = quiet();
    ASSERT_EQ(home_now_rows(&in, s_rows, HOME_NOW_ROWS), 1);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_CAUGHT_UP);
    ASSERT_STR_EQ(s_rows[0].title, "All caught up");
    ASSERT_STR_EQ(s_rows[0].detail, "No new messages or calls");
    ASSERT_STR_EQ(s_rows[0].call_back, "");
}

TEST(home_now_with_nothing_new_adds_the_mesh_when_it_is_on) {
    home_now_in_t in = quiet();
    in.mesh_running = true;
    in.mesh_contacts = 4;
    ASSERT_EQ(home_now_rows(&in, s_rows, HOME_NOW_ROWS), 2);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_CAUGHT_UP);
    ASSERT_EQ(s_rows[1].kind, HOME_NOW_MESH);
    ASSERT_STR_EQ(s_rows[1].title, "Mesh on");
    ASSERT_STR_EQ(s_rows[1].detail, "4 contacts");
}

TEST(home_now_counts_the_mesh_contacts_in_the_singular_and_none) {
    home_now_in_t in = quiet();
    in.mesh_running = true;
    in.mesh_contacts = 1;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[1].detail, "1 contact");
    in.mesh_contacts = 0;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[1].detail, "No contacts yet");
    in.mesh_contacts = -1;  /* the mesh's lock poisoned */
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[1].detail, "No contacts yet");
}

TEST(home_now_one_message_says_where_it_came_from) {
    home_now_in_t in = quiet();
    in.sms_unread = 1;
    ASSERT_EQ(home_now_rows(&in, s_rows, HOME_NOW_ROWS), 1);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_MESSAGES);
    ASSERT_STR_EQ(s_rows[0].title, "1 new message");
    ASSERT_STR_EQ(s_rows[0].detail, "SMS");
    in.sms_unread = 0;
    in.mesh_unread = 1;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].title, "1 new message");
    ASSERT_STR_EQ(s_rows[0].detail, "From the mesh");
}

TEST(home_now_messages_from_both_are_counted_apart) {
    home_now_in_t in = quiet();
    in.sms_unread = 1;
    in.mesh_unread = 1;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].title, "2 new messages");
    ASSERT_STR_EQ(s_rows[0].detail, "1 SMS" DOT "1 from the mesh");
    in.sms_unread = 3;
    in.mesh_unread = 0;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].title, "3 new messages");
    ASSERT_STR_EQ(s_rows[0].detail, "SMS");
}

TEST(home_now_tailnet_chat_messages_count_with_the_rest) {
    home_now_in_t in = quiet();
    in.chat_unread = 2;
    ASSERT_EQ(home_now_rows(&in, s_rows, HOME_NOW_ROWS), 1);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_MESSAGES);
    ASSERT_STR_EQ(s_rows[0].title, "2 new messages");
    ASSERT_STR_EQ(s_rows[0].detail, "Chat");
    ASSERT_EQ(home_now_messages_app(&in), HOME_MSG_CHAT);
    in.sms_unread = 1;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].title, "3 new messages");
    ASSERT_STR_EQ(s_rows[0].detail, "1 SMS" DOT "2 in Chat");
    ASSERT_EQ(home_now_messages_app(&in), HOME_MSG_SMS);
    in.mesh_unread = 4;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].detail, "1 SMS" DOT "4 from the mesh" DOT "2 in Chat");
}

TEST(home_now_messages_open_the_app_that_has_them) {
    home_now_in_t in = quiet();
    in.mesh_unread = 2;
    ASSERT_EQ(home_now_messages_app(&in), HOME_MSG_MESH);
    ASSERT_STR_EQ(home_msg_app_id(home_now_messages_app(&in)), "com.thistle.meshchat");
    in.chat_unread = 1;
    ASSERT_EQ(home_now_messages_app(&in), HOME_MSG_MESH);
    in.mesh_unread = 0;
    ASSERT_STR_EQ(home_msg_app_id(home_now_messages_app(&in)), "com.thistle.tailchat");
    in.sms_unread = 5;
    ASSERT_STR_EQ(home_msg_app_id(home_now_messages_app(&in)), "com.thistle.sms");
    ASSERT_EQ(home_now_messages_app(NULL), HOME_MSG_SMS);
}

TEST(home_now_a_missed_call_is_named_and_timed) {
    home_now_in_t in = quiet();
    in.missed_calls = 1;
    in.missed_number = "+4917612345678";
    in.missed_name = "Reza Amini";
    in.missed_hour = 13;
    in.missed_minute = 52;
    ASSERT_EQ(home_now_rows(&in, s_rows, HOME_NOW_ROWS), 1);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_MISSED_CALL);
    ASSERT_STR_EQ(s_rows[0].title, "Missed call");
    ASSERT_STR_EQ(s_rows[0].detail, "Reza Amini" DOT "13:52");
    ASSERT_STR_EQ(s_rows[0].call_back, "+4917612345678");
}

TEST(home_now_a_missed_call_takes_the_users_clock_format) {
    home_now_in_t in = quiet();
    in.missed_calls = 1;
    in.missed_number = "+4917612345678";
    in.missed_hour = 13;
    in.missed_minute = 5;
    in.format_24h = false;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].detail, "+4917612345678" DOT "1:05 PM");
    in.missed_hour = 0;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].detail, "+4917612345678" DOT "12:05 AM");
    in.format_24h = true;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].detail, "+4917612345678" DOT "00:05");
}

TEST(home_now_a_missed_call_with_no_time_leaves_it_out) {
    home_now_in_t in = quiet();
    in.missed_calls = 1;
    in.missed_number = "0701234567";
    in.missed_name = "";
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].detail, "0701234567");
    ASSERT_STR_EQ(s_rows[0].call_back, "0701234567");
}

TEST(home_now_a_withheld_number_has_no_call_back) {
    home_now_in_t in = quiet();
    in.missed_calls = 1;
    in.missed_number = "";
    in.missed_hour = 9;
    in.missed_minute = 0;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].detail, "Withheld" DOT "09:00");
    ASSERT_STR_EQ(s_rows[0].call_back, "");
}

TEST(home_now_several_missed_calls_name_the_last) {
    home_now_in_t in = quiet();
    in.missed_calls = 2;
    in.missed_number = "+4917612345678";
    in.missed_name = "Reza Amini";
    in.missed_hour = 13;
    in.missed_minute = 52;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].title, "2 missed calls");
    ASSERT_STR_EQ(s_rows[0].detail, "Last: Reza Amini");
    ASSERT_STR_EQ(s_rows[0].call_back, "+4917612345678");
    in.missed_number = "";
    in.missed_name = NULL;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].detail, "Last: Withheld");
    ASSERT_STR_EQ(s_rows[0].call_back, "");
}

TEST(home_now_a_missed_call_gone_from_the_log_says_only_how_many) {
    /* The count outlives a cleared log until Recents is seen. */
    home_now_in_t in = quiet();
    in.missed_calls = 1;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].title, "Missed call");
    ASSERT_STR_EQ(s_rows[0].detail, "");
    ASSERT_STR_EQ(s_rows[0].call_back, "");
    in.missed_calls = 3;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].title, "3 missed calls");
    ASSERT_STR_EQ(s_rows[0].detail, "");
}

TEST(home_now_the_tailnet_asks_for_a_sign_in_or_an_admin) {
    home_now_in_t in = quiet();
    in.tailnet = HOME_TAILNET_LOGIN;
    ASSERT_EQ(home_now_rows(&in, s_rows, HOME_NOW_ROWS), 1);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_TAILNET);
    ASSERT_STR_EQ(s_rows[0].title, "Tailscale");
    ASSERT_STR_EQ(s_rows[0].detail, "Sign in to join your tailnet");
    in.tailnet = HOME_TAILNET_APPROVAL;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].detail, "Waiting for an admin's approval");
}

TEST(home_now_shows_the_first_two_that_apply_in_order) {
    home_now_in_t in = quiet();
    in.sms_unread = 1;
    in.missed_calls = 1;
    in.missed_number = "0701234567";
    in.tailnet = HOME_TAILNET_LOGIN;
    in.mesh_running = true;
    ASSERT_EQ(home_now_rows(&in, s_rows, HOME_NOW_ROWS), 2);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_MESSAGES);
    ASSERT_EQ(s_rows[1].kind, HOME_NOW_MISSED_CALL);
    in.sms_unread = 0;
    ASSERT_EQ(home_now_rows(&in, s_rows, HOME_NOW_ROWS), 2);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_MISSED_CALL);
    ASSERT_EQ(s_rows[1].kind, HOME_NOW_TAILNET);
}

TEST(home_now_something_new_leaves_out_caught_up_and_the_mesh) {
    home_now_in_t in = quiet();
    in.tailnet = HOME_TAILNET_APPROVAL;
    in.mesh_running = true;
    in.mesh_contacts = 4;
    ASSERT_EQ(home_now_rows(&in, s_rows, HOME_NOW_ROWS), 1);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_TAILNET);
}

TEST(home_now_keeps_to_the_room_it_is_given) {
    home_now_in_t in = quiet();
    in.sms_unread = 1;
    in.missed_calls = 1;
    in.mesh_running = true;
    ASSERT_EQ(home_now_rows(&in, s_rows, 1), 1);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_MESSAGES);
    ASSERT_EQ(home_now_rows(&in, s_rows, 0), 0);
    in = quiet();
    in.mesh_running = true;
    ASSERT_EQ(home_now_rows(&in, s_rows, 1), 1);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_CAUGHT_UP);
}

TEST(home_now_the_same_readings_give_the_same_bytes) {
    /* The launcher compares two answers with memcmp() to know whether to
     * redraw: nothing left over from an earlier, longer text may differ. */
    home_now_row_t a[HOME_NOW_ROWS], b[HOME_NOW_ROWS];
    home_now_in_t in = quiet();
    in.missed_calls = 1;
    in.missed_number = "+4917612345678";
    in.missed_name = "Somebody With A Long Name";
    home_now_rows(&in, a, HOME_NOW_ROWS);
    in.missed_name = "Reza";
    home_now_rows(&in, a, HOME_NOW_ROWS);
    home_now_rows(&in, b, HOME_NOW_ROWS);
    ASSERT_TRUE(memcmp(a, b, sizeof(a)) == 0);
}

/* --- updates (ticket 09 of .scratch/ota) ---------------------------------- */

TEST(home_now_names_the_thistleos_version_on_offer) {
    home_now_in_t in = quiet();
    in.os_update = "0.6.0";
    ASSERT_EQ(home_now_rows(&in, s_rows, HOME_NOW_ROWS), 1);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_SYSTEM_UPDATE);
    ASSERT_STR_EQ(s_rows[0].title, "System update");
    ASSERT_STR_EQ(s_rows[0].detail, "ThistleOS 0.6.0");
    ASSERT_STR_EQ(s_rows[0].call_back, "");
}

TEST(home_now_counts_app_updates_alongside_the_thistleos_one) {
    home_now_in_t in = quiet();
    in.os_update = "0.6.0";
    in.app_updates = 1;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_SYSTEM_UPDATE);
    ASSERT_STR_EQ(s_rows[0].detail, "ThistleOS 0.6.0 and 1 app");
    in.app_updates = 3;
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_STR_EQ(s_rows[0].detail, "ThistleOS 0.6.0 and 3 apps");
}

TEST(home_now_app_updates_alone_open_the_app_store) {
    home_now_in_t in = quiet();
    in.app_updates = 1;
    ASSERT_EQ(home_now_rows(&in, s_rows, HOME_NOW_ROWS), 1);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_APP_UPDATES);
    ASSERT_STR_EQ(s_rows[0].title, "App updates");
    ASSERT_STR_EQ(s_rows[0].detail, "1 app has an update");
    in.app_updates = 2;
    in.os_update = "";
    home_now_rows(&in, s_rows, HOME_NOW_ROWS);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_APP_UPDATES);
    ASSERT_STR_EQ(s_rows[0].detail, "2 apps have updates");
}

TEST(home_now_updates_come_after_the_tailnet_and_before_caught_up) {
    home_now_in_t in = quiet();
    in.tailnet = HOME_TAILNET_LOGIN;
    in.os_update = "0.6.0";
    in.mesh_running = true;
    ASSERT_EQ(home_now_rows(&in, s_rows, HOME_NOW_ROWS), 2);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_TAILNET);
    ASSERT_EQ(s_rows[1].kind, HOME_NOW_SYSTEM_UPDATE);
    in.sms_unread = 1;
    ASSERT_EQ(home_now_rows(&in, s_rows, HOME_NOW_ROWS), 2);
    ASSERT_EQ(s_rows[0].kind, HOME_NOW_MESSAGES);
    ASSERT_EQ(s_rows[1].kind, HOME_NOW_TAILNET);
}

TEST(home_now_a_version_too_long_for_the_row_is_cut_not_overrun) {
    home_now_row_t a[HOME_NOW_ROWS], b[HOME_NOW_ROWS];
    home_now_in_t in = quiet();
    in.os_update = "0.6.0-rc.1-and-a-very-long-suffix-that-goes-on-and-on-and-on-and-on-and-on-and-on";
    in.app_updates = 12;
    ASSERT_EQ(home_now_rows(&in, a, HOME_NOW_ROWS), 1);
    ASSERT_EQ(strlen(a[0].detail), (size_t)HOME_NOW_DETAIL_MAX - 1);
    home_now_rows(&in, b, HOME_NOW_ROWS);
    ASSERT_TRUE(memcmp(a, b, sizeof(a)) == 0);
}
