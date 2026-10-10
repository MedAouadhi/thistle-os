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

    TEST_SUMMARY();
    return _tests_failed > 0 ? 1 : 0;
}
