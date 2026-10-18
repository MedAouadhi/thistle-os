/*
 * Simulator unit test runner.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "test_runner.h"

int _tests_run = 0;
int _tests_passed = 0;
int _tests_failed = 0;

/* Forward declarations from test files */
/* test_sim_assert.c */
extern void test_assert_uninitialized_returns_zero(void);
extern void test_assert_positive_match_passes(void);
extern void test_assert_positive_not_found_fails(void);
extern void test_assert_negative_found_fails(void);

/* test_i2c_bus.c */
extern void test_i2c_bus_init_succeeds(void);
extern void test_i2c_bus_get_out_of_range_returns_null(void);
extern void test_i2c_add_device_returns_handle(void);
extern void test_i2c_unregistered_address_returns_sentinel(void);
extern void test_pcf8563_who_am_i(void);
extern void test_pcf8563_reads_time(void);
extern void test_qmi8658c_who_am_i(void);
extern void test_qmi8658c_reads_accel_data(void);
extern void test_tca8418_empty_fifo(void);
extern void test_tca8418_key_injection(void);
extern void test_cst328_no_touch(void);
extern void test_cst328_touch_injection(void);

/* test_cst66xx.c */
extern void test_cst66xx_says_nothing_until_normal_mode_is_selected(void);
extern void test_cst66xx_reports_a_touch_in_the_documented_frame(void);
extern void test_cst66xx_checksum_covers_the_point_data_seeded_with_0x55(void);
extern void test_cst66xx_reports_a_lift_with_no_fingers(void);
extern void test_cst66xx_holds_its_report_until_the_host_releases_it(void);
extern void test_cst66xx_ignores_two_byte_addresses_a_cst328_driver_would_send(void);
extern void test_cst66xx_identifies_itself_the_way_the_real_part_did(void);

/* test_wm_select.c */
extern void test_wm_select_defaults_an_epaper_board_to_the_lvgl_epaper_manager(void);
extern void test_wm_select_defaults_an_lcd_board_to_the_lvgl_lcd_manager(void);
extern void test_wm_select_treats_empty_and_auto_as_no_preference(void);
extern void test_wm_select_honours_the_lvgl_epaper_manager_when_asked_for_it(void);
extern void test_wm_select_honours_thistle_tk_on_an_epaper_board(void);
extern void test_wm_select_honours_thistle_tk_on_a_board_that_would_default_otherwise(void);
extern void test_wm_select_falls_back_when_the_name_matches_no_manager(void);
extern void test_wm_select_never_offers_lvgl_apps_to_thistle_tk(void);

/* test_epaper_refresh_policy.c */
extern void test_epaper_policy_defaults_to_fast(void);
extern void test_epaper_policy_full_stays_full(void);
extern void test_epaper_policy_auto_cleans_after_every_run_of_fast_refreshes(void);
extern void test_epaper_policy_requested_full_happens_once_under_fast(void);
extern void test_epaper_policy_requested_full_restarts_the_auto_run(void);
extern void test_epaper_policy_unknown_value_falls_back_to_fast(void);
extern void test_epaper_policy_init_forgets_the_policy_and_any_request(void);

/* test_dictation_core.c */
extern void test_dictation_wav_header_says_16k_mono_16_bit_pcm(void);
extern void test_dictation_a_minute_fits_the_buffer_it_is_given(void);
extern void test_dictation_form_carries_the_model_the_language_and_the_file(void);
extern void test_dictation_form_refuses_values_that_would_break_it(void);
extern void test_dictation_reads_the_text_of_an_answer(void);
extern void test_dictation_reads_arabic_sent_as_it_is_or_escaped(void);
extern void test_dictation_reads_a_surrogate_pair_as_one_character(void);
extern void test_dictation_reads_escapes(void);
extern void test_dictation_finds_the_key_past_other_members_of_any_kind(void);
extern void test_dictation_cuts_a_long_text_between_characters(void);
extern void test_dictation_finds_nothing_in_what_is_not_an_answer(void);
extern void test_dictation_reads_the_message_of_a_refusal(void);
extern void test_dictation_says_what_a_refusal_means(void);
extern void test_dictation_config_defaults_are_the_hermes_agents(void);
extern void test_dictation_config_takes_what_the_card_gives(void);
extern void test_dictation_config_keeps_defaults_for_empty_or_oversized_values(void);
extern void test_dictation_config_ignores_a_file_that_is_not_json(void);
extern void test_dictation_joins_a_word_before_the_cursor_with_a_space(void);
extern void test_dictation_adds_no_space_at_the_start_or_after_one(void);
extern void test_dictation_adds_no_space_before_punctuation(void);
extern void test_dictation_takes_an_arabic_letter_before_the_cursor_for_a_word(void);
extern void test_dictation_trims_the_transcript(void);
extern void test_dictation_keeps_line_breaks_in_a_text_area_and_not_in_a_line(void);
extern void test_dictation_cuts_a_long_join_between_characters(void);
extern void test_dictation_finds_the_byte_before_a_cursor_counted_in_characters(void);

/* test_arabic_shape.c */
extern void test_arabic_letters_take_their_joined_forms(void);
extern void test_arabic_lam_and_alef_make_one_ligature(void);
extern void test_arabic_marks_and_tatweel_keep_the_join(void);
extern void test_arabic_shaping_leaves_everything_else_as_it_is(void);
extern void test_arabic_shaping_moves_positions_with_the_text(void);
extern void test_arabic_present_finds_the_arabic_block(void);
extern void test_arabic_needs_shaping_only_for_letters_not_yet_joined(void);
extern void test_arabic_logical_offset_finds_the_letters_a_place_came_from(void);
extern void test_arabic_a_place_shaped_and_found_again_is_where_it_was(void);

/* test_pull_gesture.c */
extern void test_pull_threshold_is_an_eighth_of_the_screen(void);
extern void test_pull_threshold_has_a_floor_on_a_short_screen(void);
extern void test_pull_down_from_the_bar_opens(void);
extern void test_pull_down_opens_before_the_finger_lifts(void);
extern void test_pull_down_from_below_the_bar_does_nothing(void);
extern void test_pull_too_short_does_nothing(void);
extern void test_pull_more_sideways_than_down_does_nothing(void);
extern void test_pull_a_tap_on_the_bar_does_nothing(void);
extern void test_pull_down_acts_once_per_touch(void);
extern void test_pull_down_does_not_open_an_open_panel_again(void);
extern void test_pull_up_closes_an_open_panel(void);
extern void test_pull_up_does_nothing_with_the_panel_shut(void);
extern void test_pull_a_slider_drag_does_not_close(void);
extern void test_pull_a_move_with_no_press_starts_a_touch(void);
extern void test_pull_a_lift_with_no_press_does_nothing(void);
extern void test_pull_each_touch_starts_afresh(void);

/* test_home_core.c */
extern void test_home_tdeck_max_has_the_clock_the_new_rows_and_two_rows_of_three(void);
extern void test_home_tdeck_lcd_has_one_row_of_four(void);
extern void test_home_tdisplay_gives_up_the_new_rows_first(void);
extern void test_home_cardputer_gives_up_the_clock_too(void);
extern void test_home_an_oled_has_room_for_the_bar_alone(void);
extern void test_home_a_tall_screen_stops_at_two_rows(void);
extern void test_home_cells_share_the_whole_width(void);
extern void test_home_pins_the_preferred_apps_in_their_order(void);
extern void test_home_fills_the_place_of_a_missing_app_from_the_rest(void);
extern void test_home_pins_no_more_than_asked_for(void);
extern void test_home_pins_an_app_once_though_preferred_twice(void);
extern void test_home_writes_the_date_on_one_line(void);
extern void test_home_writes_the_weekday_on_its_own_line_when_asked(void);
extern void test_home_writes_nothing_for_a_date_out_of_range(void);
extern void test_home_cuts_the_date_to_the_buffer(void);

/* test_xl9555.c */
extern void test_xl9555_resets_to_all_inputs_with_its_latches_high(void);
extern void test_xl9555_records_every_write_in_order(void);
extern void test_xl9555_log_distinguishes_configuration_writes_from_output_writes(void);
extern void test_xl9555_raising_one_rail_leaves_the_rest_of_the_port_alone(void);
extern void test_xl9555_reads_back_a_driven_pin_and_floats_the_rest_high(void);
extern void test_xl9555_never_writes_its_input_ports(void);

/* test_board_fingerprint.c */
extern void test_fingerprint_a_max_is_told_apart_from_the_pro_it_is_a_revision_of(void);
extern void test_fingerprint_a_max_is_recognised_before_its_rails_are_raised(void);
extern void test_fingerprint_a_pro_is_unaffected_by_the_max_rule(void);
extern void test_fingerprint_a_keyboard_and_an_imu_alone_do_not_name_a_pro(void);
extern void test_fingerprint_a_keyboard_and_touch_on_the_pros_pins_are_ambiguous(void);
extern void test_fingerprint_a_keyboard_and_touch_on_the_tdecks_pins_name_a_tdeck(void);
extern void test_fingerprint_a_keyboard_alone_on_the_tdecks_pins_still_names_a_tdeck(void);
extern void test_fingerprint_an_oled_on_unexpected_pins_is_ambiguous(void);
extern void test_fingerprint_an_oled_pin_pair_chooses_between_the_two_boards_that_share_it(void);
extern void test_fingerprint_a_bus_with_nothing_on_it_is_unidentified(void);
extern void test_fingerprint_a_scan_reports_whether_anything_answered(void);
extern void test_fingerprint_a_board_with_no_i2c_at_all_is_assumed_headless(void);
extern void test_fingerprint_a_match_carries_the_profile_for_the_board_it_names(void);

/* test_http_client_shim.c */
extern void test_http_shim_delivers_fixture_bytes_status_and_cleanup(void);
extern void test_http_shim_streams_a_body_in_pieces(void);
extern void test_http_shim_stream_of_a_missing_file_fails(void);

int main(void)
{
    printf("=== Simulator Unit Tests ===\n\n");

    printf("[sim_assert]\n");
    RUN_TEST(assert_uninitialized_returns_zero);
    RUN_TEST(assert_positive_match_passes);
    RUN_TEST(assert_positive_not_found_fails);
    RUN_TEST(assert_negative_found_fails);

    printf("\n[i2c_bus]\n");
    RUN_TEST(i2c_bus_init_succeeds);
    RUN_TEST(i2c_bus_get_out_of_range_returns_null);
    RUN_TEST(i2c_add_device_returns_handle);
    RUN_TEST(i2c_unregistered_address_returns_sentinel);

    printf("\n[pcf8563]\n");
    RUN_TEST(pcf8563_who_am_i);
    RUN_TEST(pcf8563_reads_time);

    printf("\n[qmi8658c]\n");
    RUN_TEST(qmi8658c_who_am_i);
    RUN_TEST(qmi8658c_reads_accel_data);

    printf("\n[tca8418]\n");
    RUN_TEST(tca8418_empty_fifo);
    RUN_TEST(tca8418_key_injection);

    printf("\n[cst328]\n");
    RUN_TEST(cst328_no_touch);
    RUN_TEST(cst328_touch_injection);

    printf("\n[cst66xx]\n");
    RUN_TEST(cst66xx_says_nothing_until_normal_mode_is_selected);
    RUN_TEST(cst66xx_reports_a_touch_in_the_documented_frame);
    RUN_TEST(cst66xx_checksum_covers_the_point_data_seeded_with_0x55);
    RUN_TEST(cst66xx_reports_a_lift_with_no_fingers);
    RUN_TEST(cst66xx_holds_its_report_until_the_host_releases_it);
    RUN_TEST(cst66xx_ignores_two_byte_addresses_a_cst328_driver_would_send);
    RUN_TEST(cst66xx_identifies_itself_the_way_the_real_part_did);

    printf("\n[wm_select]\n");
    RUN_TEST(wm_select_defaults_an_epaper_board_to_the_lvgl_epaper_manager);
    RUN_TEST(wm_select_defaults_an_lcd_board_to_the_lvgl_lcd_manager);
    RUN_TEST(wm_select_treats_empty_and_auto_as_no_preference);
    RUN_TEST(wm_select_honours_the_lvgl_epaper_manager_when_asked_for_it);
    RUN_TEST(wm_select_honours_thistle_tk_on_an_epaper_board);
    RUN_TEST(wm_select_honours_thistle_tk_on_a_board_that_would_default_otherwise);
    RUN_TEST(wm_select_falls_back_when_the_name_matches_no_manager);
    RUN_TEST(wm_select_never_offers_lvgl_apps_to_thistle_tk);

    printf("\n[epaper_refresh_policy]\n");
    RUN_TEST(epaper_policy_defaults_to_fast);
    RUN_TEST(epaper_policy_full_stays_full);
    RUN_TEST(epaper_policy_auto_cleans_after_every_run_of_fast_refreshes);
    RUN_TEST(epaper_policy_requested_full_happens_once_under_fast);
    RUN_TEST(epaper_policy_requested_full_restarts_the_auto_run);
    RUN_TEST(epaper_policy_unknown_value_falls_back_to_fast);
    RUN_TEST(epaper_policy_init_forgets_the_policy_and_any_request);

    printf("\n[dictation_core]\n");
    RUN_TEST(dictation_wav_header_says_16k_mono_16_bit_pcm);
    RUN_TEST(dictation_a_minute_fits_the_buffer_it_is_given);
    RUN_TEST(dictation_form_carries_the_model_the_language_and_the_file);
    RUN_TEST(dictation_form_refuses_values_that_would_break_it);
    RUN_TEST(dictation_reads_the_text_of_an_answer);
    RUN_TEST(dictation_reads_arabic_sent_as_it_is_or_escaped);
    RUN_TEST(dictation_reads_a_surrogate_pair_as_one_character);
    RUN_TEST(dictation_reads_escapes);
    RUN_TEST(dictation_finds_the_key_past_other_members_of_any_kind);
    RUN_TEST(dictation_cuts_a_long_text_between_characters);
    RUN_TEST(dictation_finds_nothing_in_what_is_not_an_answer);
    RUN_TEST(dictation_reads_the_message_of_a_refusal);
    RUN_TEST(dictation_says_what_a_refusal_means);
    RUN_TEST(dictation_config_defaults_are_the_hermes_agents);
    RUN_TEST(dictation_config_takes_what_the_card_gives);
    RUN_TEST(dictation_config_keeps_defaults_for_empty_or_oversized_values);
    RUN_TEST(dictation_config_ignores_a_file_that_is_not_json);
    RUN_TEST(dictation_joins_a_word_before_the_cursor_with_a_space);
    RUN_TEST(dictation_adds_no_space_at_the_start_or_after_one);
    RUN_TEST(dictation_adds_no_space_before_punctuation);
    RUN_TEST(dictation_takes_an_arabic_letter_before_the_cursor_for_a_word);
    RUN_TEST(dictation_trims_the_transcript);
    RUN_TEST(dictation_keeps_line_breaks_in_a_text_area_and_not_in_a_line);
    RUN_TEST(dictation_cuts_a_long_join_between_characters);
    RUN_TEST(dictation_finds_the_byte_before_a_cursor_counted_in_characters);

    printf("\n[arabic_shape]\n");
    RUN_TEST(arabic_letters_take_their_joined_forms);
    RUN_TEST(arabic_lam_and_alef_make_one_ligature);
    RUN_TEST(arabic_marks_and_tatweel_keep_the_join);
    RUN_TEST(arabic_shaping_leaves_everything_else_as_it_is);
    RUN_TEST(arabic_shaping_moves_positions_with_the_text);
    RUN_TEST(arabic_present_finds_the_arabic_block);
    RUN_TEST(arabic_needs_shaping_only_for_letters_not_yet_joined);
    RUN_TEST(arabic_logical_offset_finds_the_letters_a_place_came_from);
    RUN_TEST(arabic_a_place_shaped_and_found_again_is_where_it_was);

    printf("\n[pull_gesture]\n");
    RUN_TEST(pull_threshold_is_an_eighth_of_the_screen);
    RUN_TEST(pull_threshold_has_a_floor_on_a_short_screen);
    RUN_TEST(pull_down_from_the_bar_opens);
    RUN_TEST(pull_down_opens_before_the_finger_lifts);
    RUN_TEST(pull_down_from_below_the_bar_does_nothing);
    RUN_TEST(pull_too_short_does_nothing);
    RUN_TEST(pull_more_sideways_than_down_does_nothing);
    RUN_TEST(pull_a_tap_on_the_bar_does_nothing);
    RUN_TEST(pull_down_acts_once_per_touch);
    RUN_TEST(pull_down_does_not_open_an_open_panel_again);
    RUN_TEST(pull_up_closes_an_open_panel);
    RUN_TEST(pull_up_does_nothing_with_the_panel_shut);
    RUN_TEST(pull_a_slider_drag_does_not_close);
    RUN_TEST(pull_a_move_with_no_press_starts_a_touch);
    RUN_TEST(pull_a_lift_with_no_press_does_nothing);
    RUN_TEST(pull_each_touch_starts_afresh);

    printf("\n[home_core]\n");
    RUN_TEST(home_tdeck_max_has_the_clock_the_new_rows_and_two_rows_of_three);
    RUN_TEST(home_tdeck_lcd_has_one_row_of_four);
    RUN_TEST(home_tdisplay_gives_up_the_new_rows_first);
    RUN_TEST(home_cardputer_gives_up_the_clock_too);
    RUN_TEST(home_an_oled_has_room_for_the_bar_alone);
    RUN_TEST(home_a_tall_screen_stops_at_two_rows);
    RUN_TEST(home_cells_share_the_whole_width);
    RUN_TEST(home_pins_the_preferred_apps_in_their_order);
    RUN_TEST(home_fills_the_place_of_a_missing_app_from_the_rest);
    RUN_TEST(home_pins_no_more_than_asked_for);
    RUN_TEST(home_pins_an_app_once_though_preferred_twice);
    RUN_TEST(home_writes_the_date_on_one_line);
    RUN_TEST(home_writes_the_weekday_on_its_own_line_when_asked);
    RUN_TEST(home_writes_nothing_for_a_date_out_of_range);
    RUN_TEST(home_cuts_the_date_to_the_buffer);

    printf("\n[xl9555]\n");
    RUN_TEST(xl9555_resets_to_all_inputs_with_its_latches_high);
    RUN_TEST(xl9555_records_every_write_in_order);
    RUN_TEST(xl9555_log_distinguishes_configuration_writes_from_output_writes);
    RUN_TEST(xl9555_raising_one_rail_leaves_the_rest_of_the_port_alone);
    RUN_TEST(xl9555_reads_back_a_driven_pin_and_floats_the_rest_high);
    RUN_TEST(xl9555_never_writes_its_input_ports);

    printf("\n[board_fingerprint]\n");
    RUN_TEST(fingerprint_a_max_is_told_apart_from_the_pro_it_is_a_revision_of);
    RUN_TEST(fingerprint_a_max_is_recognised_before_its_rails_are_raised);
    RUN_TEST(fingerprint_a_pro_is_unaffected_by_the_max_rule);
    RUN_TEST(fingerprint_a_keyboard_and_an_imu_alone_do_not_name_a_pro);
    RUN_TEST(fingerprint_a_keyboard_and_touch_on_the_pros_pins_are_ambiguous);
    RUN_TEST(fingerprint_a_keyboard_and_touch_on_the_tdecks_pins_name_a_tdeck);
    RUN_TEST(fingerprint_a_keyboard_alone_on_the_tdecks_pins_still_names_a_tdeck);
    RUN_TEST(fingerprint_an_oled_on_unexpected_pins_is_ambiguous);
    RUN_TEST(fingerprint_an_oled_pin_pair_chooses_between_the_two_boards_that_share_it);
    RUN_TEST(fingerprint_a_bus_with_nothing_on_it_is_unidentified);
    RUN_TEST(fingerprint_a_scan_reports_whether_anything_answered);
    RUN_TEST(fingerprint_a_board_with_no_i2c_at_all_is_assumed_headless);
    RUN_TEST(fingerprint_a_match_carries_the_profile_for_the_board_it_names);

    printf("\n[http_client_shim]\n");
    RUN_TEST(http_shim_delivers_fixture_bytes_status_and_cleanup);
    RUN_TEST(http_shim_streams_a_body_in_pieces);
    RUN_TEST(http_shim_stream_of_a_missing_file_fails);

    TEST_SUMMARY();
    return _tests_failed > 0 ? 1 : 0;
}
