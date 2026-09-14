/*
 * Unit tests for board fingerprint matching — which board a freshly flashed
 * device decides it is from what answered on its I2C bus.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "test_runner.h"
#include "thistle/board_fingerprint.h"

/* Everything a T-Deck Pro answers to, on the pins it answers on. */
static board_scan_t tdeck_pro_scan(void)
{
    board_scan_t scan = { 0 };
    scan.sda = 13;
    scan.scl = 14;
    scan.has_tca8418 = true;
    scan.has_bhi260ap = true;
    scan.has_cst328 = true;
    return scan;
}

/* A T-Deck Max is a T-Deck Pro plus the XL9555 IO expander. */
static board_scan_t tdeck_max_scan(void)
{
    board_scan_t scan = tdeck_pro_scan();
    scan.has_xl9555 = true;
    return scan;
}

/* The same Max as detection actually meets it: the expander has not been
 * driven yet, so the parts on the rails behind it have not come up. */
static board_scan_t unpowered_tdeck_max_scan(void)
{
    board_scan_t scan = { 0 };
    scan.sda = 13;
    scan.scl = 14;
    scan.has_xl9555 = true;
    return scan;
}

static const char *matched_id(const board_scan_t *scan)
{
    const board_profile_t *profile = board_fingerprint_match(scan);
    return profile ? profile->board_id : "unidentified";
}

/* The bug this guards: the Max shares the Pro's keyboard and IMU on the same
 * pins, and used to come up confidently as a Pro. */
TEST(fingerprint_a_max_is_told_apart_from_the_pro_it_is_a_revision_of)
{
    board_scan_t scan = tdeck_max_scan();
    ASSERT_STR_EQ(matched_id(&scan), "tdeck-max");
}

/* Detection runs before the expander phase, so the Max's IMU and touch sit on
 * rails that are still down. The expander alone has to be enough. */
TEST(fingerprint_a_max_is_recognised_before_its_rails_are_raised)
{
    board_scan_t scan = unpowered_tdeck_max_scan();
    ASSERT_STR_EQ(matched_id(&scan), "tdeck-max");
}

TEST(fingerprint_a_pro_is_unaffected_by_the_max_rule)
{
    board_scan_t scan = tdeck_pro_scan();
    ASSERT_STR_EQ(matched_id(&scan), "tdeck-pro");
}

/* A Max whose expander did not answer must not fall through to the Pro. */
TEST(fingerprint_a_keyboard_and_an_imu_alone_do_not_name_a_pro)
{
    board_scan_t scan = { 0 };
    scan.sda = 99;
    scan.scl = 98;
    scan.has_tca8418 = true;
    scan.has_bhi260ap = true;
    ASSERT_STR_EQ(matched_id(&scan), "unidentified");
}

/* A Pro or a Max whose IMU and expander both stayed quiet is not a T-Deck. */
TEST(fingerprint_a_keyboard_and_touch_on_the_pros_pins_are_ambiguous)
{
    board_scan_t scan = { 0 };
    scan.sda = 13;
    scan.scl = 14;
    scan.has_tca8418 = true;
    scan.has_cst328 = true;
    ASSERT_STR_EQ(matched_id(&scan), "unidentified");
}

TEST(fingerprint_a_keyboard_and_touch_on_the_tdecks_pins_name_a_tdeck)
{
    board_scan_t scan = { 0 };
    scan.sda = 18;
    scan.scl = 8;
    scan.has_tca8418 = true;
    scan.has_cst328 = true;
    ASSERT_STR_EQ(matched_id(&scan), "tdeck");
}

/* A T-Deck whose touch controller stayed quiet detected before and still does:
 * on the T-Deck's own pins a keyboard is not ambiguous with anything. */
TEST(fingerprint_a_keyboard_alone_on_the_tdecks_pins_still_names_a_tdeck)
{
    board_scan_t scan = { 0 };
    scan.sda = 18;
    scan.scl = 8;
    scan.has_tca8418 = true;
    ASSERT_STR_EQ(matched_id(&scan), "tdeck");
}

TEST(fingerprint_an_oled_on_unexpected_pins_is_ambiguous)
{
    board_scan_t scan = { 0 };
    scan.sda = 21;
    scan.scl = 22;
    scan.has_ssd1306 = true;
    ASSERT_STR_EQ(matched_id(&scan), "unidentified");
}

TEST(fingerprint_an_oled_pin_pair_chooses_between_the_two_boards_that_share_it)
{
    board_scan_t heltec = { 0 };
    heltec.sda = 17;
    heltec.scl = 18;
    heltec.has_ssd1306 = true;
    ASSERT_STR_EQ(matched_id(&heltec), "heltec-v3");

    board_scan_t t3s3 = { 0 };
    t3s3.sda = 18;
    t3s3.scl = 17;
    t3s3.has_ssd1306 = true;
    ASSERT_STR_EQ(matched_id(&t3s3), "t3-s3");
}

TEST(fingerprint_a_bus_with_nothing_on_it_is_unidentified)
{
    board_scan_t scan = { 0 };
    scan.sda = 18;
    scan.scl = 8;
    ASSERT_STR_EQ(matched_id(&scan), "unidentified");
}

TEST(fingerprint_a_scan_reports_whether_anything_answered)
{
    board_scan_t empty = { 0 };
    empty.sda = 18;
    empty.scl = 8;
    ASSERT_TRUE(!board_scan_found_anything(&empty));

    board_scan_t pro = tdeck_pro_scan();
    ASSERT_TRUE(board_scan_found_anything(&pro));
}

/* A board where nothing answered anywhere is assumed headless, which is a
 * separate decision from matching a scan. */
TEST(fingerprint_a_board_with_no_i2c_at_all_is_assumed_headless)
{
    ASSERT_STR_EQ(board_fingerprint_headless()->board_id, "rak3312");
}

/* The profile written to SPIFFS has to be the one the match names. */
TEST(fingerprint_a_match_carries_the_profile_for_the_board_it_names)
{
    board_scan_t scan = tdeck_max_scan();
    const board_profile_t *profile = board_fingerprint_match(&scan);
    ASSERT_TRUE(profile != NULL);
    ASSERT_STR_CONTAINS(profile->json, "\"board_id\": \"tdeck-max\"");
    ASSERT_STR_CONTAINS(profile->json, "\"io_expanders\"");
}
