/*
 * test_ota.c — Unit tests for the ThistleOS OTA subsystem
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Only hardware-independent aspects of the OTA API are tested here.
 * ota_init() itself touches the ESP-IDF OTA partition API and is therefore
 * not called in this suite — the two functions under test (get_current_version
 * and sd_update_available) do not require prior init.
 *
 * ota_apply_from_sd() and ota_apply_from_http() write flash and/or connect to
 * the network; they are excluded from unit testing.
 */

#include "unity.h"
#include "thistle/ota.h"
#include "esp_app_desc.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * test_ota_get_version
 * -------------------------------------------------------------------------- */

TEST_CASE("test_ota_get_version: the image description's version", "[ota]")
{
    /* Both are ESP-IDF's PROJECT_VER, from tools/firmware_version.sh. */
    const char *ver = ota_get_current_version();
    TEST_ASSERT_NOT_NULL(ver);
    TEST_ASSERT_EQUAL_STRING(esp_app_get_description()->version, ver);
}

/* --------------------------------------------------------------------------
 * test_ota_sd_update_not_available
 * -------------------------------------------------------------------------- */

TEST_CASE("test_ota_sd_update_not_available: no SD card in test environment", "[ota]")
{
    /*
     * ota_sd_update_available() uses stat(3) to check for
     * /sdcard/update/thistle_os.bin. The SD card is not mounted in the
     * test environment, so this must return false.
     */
    bool available = ota_sd_update_available();
    TEST_ASSERT_FALSE(available);
}
