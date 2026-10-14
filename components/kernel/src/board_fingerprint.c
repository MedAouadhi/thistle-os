/*
 * Board fingerprint matching, and the embedded fallback profiles behind it.
 *
 * Each profile below duplicates the board profile of the same board id in the
 * catalogue under sdcard_layout/config/boards. It is the only configuration a
 * freshly flashed board ever sees, so tools/validate_board_catalog.py checks
 * every one against the board profile it duplicates in CI — keep the two in
 * step rather than editing one of them.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "thistle/board_fingerprint.h"

#include <stddef.h>
#include <string.h>

/* ── Embedded fallback profiles ───────────────────────────────────────────── */

static const char JSON_TDECK[] =
"{\n"
"    \"board\": {\"name\": \"LilyGo T-Deck\", \"arch\": \"esp32s3\", \"board_id\": \"tdeck\", \"version\": \"1.0\"},\n"
"    \"buses\": {\n"
"        \"spi\": [{\"host\": 2, \"mosi\": 41, \"miso\": 38, \"sclk\": 40, \"max_transfer_bytes\": 9600}],\n"
"        \"i2c\": [{\"port\": 0, \"sda\": 18, \"scl\": 8, \"freq_hz\": 400000}]\n"
"    },\n"
"    \"drivers\": [\n"
"        {\"id\": \"com.thistle.drv.lcd-st7789\", \"hal\": \"display\", \"entry\": \"lcd-st7789.drv.elf\", \"config\": {\"cs\": 12, \"dc\": 11, \"rst\": -1, \"bl\": 15, \"spi_bus\": 0, \"width\": 320, \"height\": 240}},\n"
"        {\"id\": \"com.thistle.drv.kbd-tca8418\", \"hal\": \"input\", \"entry\": \"kbd-tca8418.drv.elf\", \"config\": {\"i2c_bus\": 0, \"i2c_addr\": \"0x34\", \"pin_int\": 46}},\n"
"        {\"id\": \"com.thistle.drv.touch-cst328\", \"hal\": \"input\", \"entry\": \"touch-cst328.drv.elf\", \"config\": {\"i2c_bus\": 0, \"i2c_addr\": \"0x1A\", \"pin_int\": 16, \"pin_rst\": -1, \"max_x\": 320, \"max_y\": 240}},\n"
"        {\"id\": \"com.thistle.drv.radio-sx1262\", \"hal\": \"radio\", \"entry\": \"radio-sx1262.drv.elf\", \"config\": {\"spi_bus\": 0, \"cs\": 7, \"dio1\": 33, \"rst\": 8, \"busy\": 34}},\n"
"        {\"id\": \"com.thistle.drv.gps-mia-m10q\", \"hal\": \"gps\", \"entry\": \"gps-mia-m10q.drv.elf\", \"config\": {\"uart_num\": 1, \"tx\": 43, \"rx\": 44, \"baud_rate\": 9600}},\n"
"        {\"id\": \"com.thistle.drv.power-tp4065b\", \"hal\": \"power\", \"entry\": \"power-tp4065b.drv.elf\", \"config\": {\"adc_pin\": 4, \"charge_pin\": 10}}\n"
"    ]\n"
"}\n";

static const char JSON_TDECK_PRO[] =
"{\n"
"    \"board\": {\"name\": \"LilyGo T-Deck Pro\", \"arch\": \"esp32s3\", \"board_id\": \"tdeck-pro\", \"version\": \"1.0\"},\n"
"    \"gpio_outputs\": [\n"
"        {\"gpio\": 38, \"level\": 1, \"delay_ms\": 100},\n"
"        {\"gpio\": 39, \"level\": 1, \"delay_ms\": 0},\n"
"        {\"gpio\": 41, \"level\": 1, \"delay_ms\": 0},\n"
"        {\"gpio\": 46, \"level\": 1, \"delay_ms\": 0},\n"
"        {\"gpio\": 2, \"level\": 0, \"delay_ms\": 0}\n"
"    ],\n"
"    \"buses\": {\n"
"        \"spi\": [{\"host\": 2, \"mosi\": 33, \"miso\": 47, \"sclk\": 36, \"max_transfer_bytes\": 9600}],\n"
"        \"i2c\": [{\"port\": 0, \"sda\": 13, \"scl\": 14, \"freq_hz\": 400000}]\n"
"    },\n"
"    \"drivers\": [\n"
"        {\"id\": \"com.thistle.drv.epaper-gdeq031t10\", \"hal\": \"display\", \"entry\": \"epaper-gdeq031t10.drv.elf\", \"config\": {\"spi_bus\": 0, \"cs\": 34, \"dc\": 35, \"rst\": -1, \"busy\": 37, \"spi_clock_hz\": 2000000}},\n"
"        {\"id\": \"com.thistle.drv.kbd-tca8418\", \"hal\": \"input\", \"entry\": \"kbd-tca8418.drv.elf\", \"config\": {\"i2c_bus\": 0, \"i2c_addr\": \"0x34\", \"pin_int\": 15}},\n"
"        {\"id\": \"com.thistle.drv.touch-cst328\", \"hal\": \"input\", \"entry\": \"touch-cst328.drv.elf\", \"config\": {\"i2c_bus\": 0, \"i2c_addr\": \"0x1A\", \"pin_int\": 12, \"pin_rst\": 45, \"max_x\": 240, \"max_y\": 320}},\n"
"        {\"id\": \"com.thistle.drv.radio-sx1262\", \"hal\": \"radio\", \"entry\": \"radio-sx1262.drv.elf\", \"config\": {\"spi_bus\": 0, \"cs\": 3, \"dio1\": 5, \"rst\": 4, \"busy\": 6}},\n"
"        {\"id\": \"com.thistle.drv.gps-mia-m10q\", \"hal\": \"gps\", \"entry\": \"gps-mia-m10q.drv.elf\", \"config\": {\"uart_num\": 2, \"tx\": 43, \"rx\": 44, \"baud_rate\": 9600}},\n"
"        {\"id\": \"com.thistle.drv.audio-pcm5102a\", \"hal\": \"audio\", \"entry\": \"audio-pcm5102a.drv.elf\", \"config\": {\"i2s_bck\": 7, \"i2s_ws\": 9, \"i2s_data\": 8}},\n"
"        {\"id\": \"com.thistle.drv.imu-bhi260ap\", \"hal\": \"imu\", \"entry\": \"imu-bhi260ap.drv.elf\", \"config\": {\"i2c_bus\": 0, \"i2c_addr\": \"0x28\", \"pin_int\": 21}},\n"
"        {\"id\": \"com.thistle.drv.power-sy6970\", \"hal\": \"power\", \"entry\": \"power-sy6970.drv.elf\", \"config\": {\"i2c_bus\": 0, \"gauge_i2c_addr\": \"0x55\"}},\n"
"        {\"id\": \"com.thistle.drv.sdcard\", \"hal\": \"storage\", \"entry\": \"sdcard.drv.elf\", \"config\": {\"spi_bus\": 0, \"pin_cs\": 48, \"mount_point\": \"/sdcard\"}}\n"
"    ]\n"
"}\n";

static const char JSON_TDECK_MAX[] =
"{\n"
"    \"board\": {\"name\": \"LilyGo T-Deck Pro Max\", \"arch\": \"esp32s3\", \"board_id\": \"tdeck-max\", \"version\": \"1.0\"},\n"
"    \"buses\": {\n"
"        \"spi\": [{\"host\": 2, \"mosi\": 33, \"miso\": 47, \"sclk\": 36, \"max_transfer_bytes\": 9600}],\n"
"        \"i2c\": [{\"port\": 0, \"sda\": 13, \"scl\": 14, \"freq_hz\": 400000}]\n"
"    },\n"
"    \"io_expanders\": [\n"
"        {\"id\": \"com.thistle.drv.ioexp-xl9555\", \"i2c_bus\": 0, \"i2c_addr\": \"0x20\"}\n"
"    ],\n"
"    \"expander_outputs\": [\n"
"        {\"expander\": 0, \"pin\": 2, \"direction\": \"output\", \"level\": 1, \"delay_ms\": 10},\n"
"        {\"expander\": 0, \"pin\": 1, \"direction\": \"output\", \"level\": 1, \"delay_ms\": 10},\n"
"        {\"expander\": 0, \"pin\": 3, \"direction\": \"output\", \"level\": 1, \"delay_ms\": 10},\n"
"        {\"expander\": 0, \"pin\": 7, \"direction\": \"output\", \"level\": 0, \"delay_ms\": 10},\n"
"        {\"expander\": 0, \"pin\": 9, \"direction\": \"output\", \"level\": 0, \"delay_ms\": 10},\n"
"        {\"expander\": 0, \"pin\": 7, \"direction\": \"output\", \"level\": 1, \"delay_ms\": 50},\n"
"        {\"expander\": 0, \"pin\": 9, \"direction\": \"output\", \"level\": 1, \"delay_ms\": 50},\n"
"        {\"expander\": 0, \"pin\": 6, \"direction\": \"output\", \"level\": 0, \"delay_ms\": 0},\n"
"        {\"expander\": 0, \"pin\": 10, \"direction\": \"output\", \"level\": 0, \"delay_ms\": 0}\n"
"    ],\n"
"    \"radio_antenna_switch\": {\"expander\": 0, \"pin\": 4, \"internal_level\": 1},\n"
"    \"power\": {\n"
"        \"key\": {\"gpio\": 0, \"active_level\": 0},\n"
"        \"wake\": [\n"
"            {\"source\": \"keyboard\", \"gpio\": 15, \"active_level\": 0},\n"
"            {\"source\": \"modem\", \"gpio\": 7, \"active_level\": 0},\n"
"            {\"source\": \"radio\", \"gpio\": 5, \"active_level\": 1}\n"
"        ],\n"
"        \"deep_sleep_off\": [\n"
"            {\"expander\": 0, \"pin\": 1}, {\"expander\": 0, \"pin\": 2}, {\"expander\": 0, \"pin\": 3},\n"
"            {\"expander\": 0, \"pin\": 5}, {\"expander\": 0, \"pin\": 6}\n"
"        ]\n"
"    },\n"
"    \"drivers\": [\n"
"        {\"id\": \"com.thistle.drv.epaper-gdeq031t10\", \"hal\": \"display\", \"entry\": \"epaper-gdeq031t10.drv.elf\", \"config\": {\"spi_bus\": 0, \"cs\": 34, \"dc\": 35, \"rst\": 9, \"busy\": 37, \"bl\": 41, \"spi_clock_hz\": 2000000, \"rotation\": 180}},\n"
"        {\"id\": \"com.thistle.drv.kbd-tca8418\", \"hal\": \"input\", \"entry\": \"kbd-tca8418.drv.elf\", \"config\": {\"i2c_bus\": 0, \"i2c_addr\": \"0x34\", \"pin_int\": 15, \"pin_backlight\": 42}},\n"
"        {\"id\": \"com.thistle.drv.touch-cst66xx\", \"hal\": \"input\", \"entry\": \"touch-cst66xx.drv.elf\", \"config\": {\"i2c_bus\": 0, \"i2c_addr\": \"0x1A\", \"pin_int\": 12, \"pin_rst\": -1, \"rst_expander\": 0, \"rst_expander_pin\": 7, \"max_x\": 240, \"max_y\": 320, \"home_key\": 0, \"back_key\": 2, \"quick_key\": 1}},\n"
"        {\"id\": \"com.thistle.drv.gps-mia-m10q\", \"hal\": \"gps\", \"entry\": \"gps-mia-m10q.drv.elf\", \"config\": {\"uart_num\": 2, \"tx\": 16, \"rx\": 2, \"baud_rate\": 38400}},\n"
"        {\"id\": \"com.thistle.drv.radio-sx1262\", \"hal\": \"radio\", \"entry\": \"radio-sx1262.drv.elf\", \"config\": {\"spi_bus\": 0, \"cs\": 3, \"dio1\": 5, \"rst\": 4, \"busy\": 6, \"freq_hz\": 868000000, \"tcxo_mv\": 2400, \"dio2_rf_switch\": true}},\n"
"        {\"id\": \"com.thistle.drv.imu-bhi260ap\", \"hal\": \"imu\", \"entry\": \"imu-bhi260ap.drv.elf\", \"config\": {\"i2c_bus\": 0, \"i2c_addr\": \"0x28\", \"pin_int\": 21}},\n"
"        {\"id\": \"com.thistle.drv.power-sy6970\", \"hal\": \"power\", \"entry\": \"power-sy6970.drv.elf\", \"config\": {\"i2c_bus\": 0, \"gauge_i2c_addr\": \"0x55\", \"battery_capacity_mah\": 1400}},\n"
"        {\"id\": \"com.thistle.drv.audio-es8311\", \"hal\": \"audio\", \"entry\": \"audio-es8311.drv.elf\", \"config\": {\"i2c_bus\": 0, \"i2c_addr\": \"0x18\", \"i2s_num\": 0, \"i2s_mclk\": 38, \"i2s_bck\": 39, \"i2s_ws\": 18, \"i2s_data\": 40, \"i2s_din\": 17, \"amp_expander\": 0, \"amp_expander_pin\": 6}},\n"
"        {\"id\": \"com.thistle.drv.sdcard\", \"hal\": \"storage\", \"entry\": \"sdcard.drv.elf\", \"config\": {\"spi_bus\": 0, \"pin_cs\": 48, \"mount_point\": \"/sdcard\"}},\n"
"        {\"id\": \"com.thistle.drv.modem-a7682e\", \"hal\": \"network\", \"entry\": \"modem-a7682e.drv.elf\", \"config\": {\"uart_num\": 1, \"tx\": 10, \"rx\": 11, \"baud_rate\": 115200, \"fast_baud_rate\": 921600, \"dtr\": 8, \"ri\": 7, \"power_expander\": 0, \"power_expander_pin\": 0, \"pwrkey_expander\": 0, \"pwrkey_expander_pin\": 8, \"pwrkey_press_level\": 1}},\n"
"        {\"id\": \"com.thistle.drv.haptic-drv2605\", \"hal\": \"haptic\", \"entry\": \"haptic-drv2605.drv.elf\", \"config\": {\"i2c_bus\": 0, \"i2c_addr\": \"0x5A\", \"en_expander\": 0, \"en_expander_pin\": 5}}\n"
"    ]\n"
"}\n";

static const char JSON_TDISPLAY[] =
"{\n"
"    \"board\": {\"name\": \"LilyGo T-Display-S3\", \"arch\": \"esp32s3\", \"board_id\": \"tdisplay-s3\", \"version\": \"1.0\"},\n"
"    \"buses\": {\n"
"        \"i2c\": [{\"port\": 0, \"sda\": 18, \"scl\": 17, \"freq_hz\": 400000}]\n"
"    },\n"
"    \"drivers\": [\n"
"        {\"id\": \"com.thistle.drv.lcd-st7789\", \"hal\": \"display\", \"entry\": \"lcd-st7789.drv.elf\", \"config\": {\"cs\": 6, \"dc\": 7, \"rst\": 5, \"bl\": 38, \"spi_bus\": 0, \"width\": 170, \"height\": 320}},\n"
"        {\"id\": \"com.thistle.drv.touch-cst816\", \"hal\": \"input\", \"entry\": \"touch-cst816.drv.elf\", \"config\": {\"i2c_bus\": 0, \"i2c_addr\": \"0x15\", \"pin_int\": 16, \"pin_rst\": 21, \"max_x\": 170, \"max_y\": 320}}\n"
"    ]\n"
"}\n";

static const char JSON_CARDPUTER[] =
"{\n"
"    \"board\": {\"name\": \"M5Stack Cardputer\", \"arch\": \"esp32s3\", \"board_id\": \"cardputer\", \"version\": \"1.0\"},\n"
"    \"buses\": {\n"
"        \"spi\": [{\"host\": 2, \"mosi\": 35, \"miso\": -1, \"sclk\": 36, \"max_transfer_bytes\": 4096}],\n"
"        \"i2c\": [{\"port\": 0, \"sda\": 1, \"scl\": 2, \"freq_hz\": 400000}]\n"
"    },\n"
"    \"drivers\": [\n"
"        {\"id\": \"com.thistle.drv.lcd-st7789\", \"hal\": \"display\", \"entry\": \"lcd-st7789.drv.elf\", \"config\": {\"cs\": 37, \"dc\": 34, \"rst\": 33, \"bl\": 38, \"spi_bus\": 0, \"width\": 240, \"height\": 135, \"x_offset\": 40, \"y_offset\": 52}},\n"
"        {\"id\": \"com.thistle.drv.kbd-cardkb\", \"hal\": \"input\", \"entry\": \"kbd-cardkb.drv.elf\", \"config\": {\"i2c_bus\": 0, \"i2c_addr\": \"0x5F\"}},\n"
"        {\"id\": \"com.thistle.drv.sdcard\", \"hal\": \"storage\", \"entry\": \"sdcard.drv.elf\", \"config\": {\"spi_host\": 1, \"spi_mosi\": 14, \"spi_miso\": 39, \"spi_sclk\": 40, \"pin_cs\": 12, \"mount_point\": \"/sdcard\"}}\n"
"    ]\n"
"}\n";

static const char JSON_HELTEC_V3[] =
"{\n"
"    \"board\": {\"name\": \"Heltec WiFi LoRa 32 V3\", \"arch\": \"esp32s3\", \"board_id\": \"heltec-v3\", \"version\": \"1.0\"},\n"
"    \"buses\": {\n"
"        \"spi\": [{\"host\": 2, \"mosi\": 10, \"miso\": 11, \"sclk\": 9, \"max_transfer_bytes\": 4096}],\n"
"        \"i2c\": [{\"port\": 0, \"sda\": 17, \"scl\": 18, \"freq_hz\": 400000}]\n"
"    },\n"
"    \"drivers\": [\n"
"        {\"id\": \"com.thistle.drv.oled-ssd1306\", \"hal\": \"display\", \"entry\": \"oled-ssd1306.drv.elf\", \"config\": {\"i2c_bus\": 0, \"i2c_addr\": \"0x3C\", \"pin_rst\": 21, \"pin_vext\": 36}},\n"
"        {\"id\": \"com.thistle.drv.radio-sx1262\", \"hal\": \"radio\", \"entry\": \"radio-sx1262.drv.elf\", \"config\": {\"spi_bus\": 0, \"cs\": 8, \"dio1\": 14, \"rst\": 12, \"busy\": 13, \"tcxo\": true}},\n"
"        {\"id\": \"com.thistle.drv.power-heltec-v3\", \"hal\": \"power\", \"entry\": \"power-tp4065b.drv.elf\", \"config\": {\"adc_pin\": 1, \"charge_pin\": -1, \"adc_ctrl_pin\": 37}}\n"
"    ]\n"
"}\n";

static const char JSON_T3_S3[] =
"{\n"
"    \"board\": {\"name\": \"LilyGo T3-S3 LoRa\", \"arch\": \"esp32s3\", \"board_id\": \"t3-s3\", \"version\": \"1.0\"},\n"
"    \"buses\": {\n"
"        \"spi\": [{\"host\": 2, \"mosi\": 11, \"miso\": 2, \"sclk\": 14, \"max_transfer_bytes\": 4096}],\n"
"        \"i2c\": [{\"port\": 0, \"sda\": 18, \"scl\": 17, \"freq_hz\": 400000}]\n"
"    },\n"
"    \"drivers\": [\n"
"        {\"id\": \"com.thistle.drv.oled-ssd1306\", \"hal\": \"display\", \"entry\": \"oled-ssd1306.drv.elf\", \"config\": {\"i2c_bus\": 0, \"i2c_addr\": \"0x3C\", \"pin_rst\": -1}},\n"
"        {\"id\": \"com.thistle.drv.radio-sx1262\", \"hal\": \"radio\", \"entry\": \"radio-sx1262.drv.elf\", \"config\": {\"spi_bus\": 0, \"cs\": 8, \"dio1\": 33, \"rst\": 12, \"busy\": 34}}\n"
"    ]\n"
"}\n";

static const char JSON_RAK3312[] =
"{\n"
"    \"board\": {\"name\": \"RAK WisBlock RAK3312\", \"arch\": \"esp32s3\", \"board_id\": \"rak3312\", \"version\": \"1.0\"},\n"
"    \"buses\": {\n"
"        \"spi\": [],\n"
"        \"i2c\": [{\"port\": 0, \"sda\": 4, \"scl\": 5, \"freq_hz\": 400000}]\n"
"    },\n"
"    \"drivers\": [\n"
"        {\"id\": \"com.thistle.drv.radio-sx1262\", \"hal\": \"radio\", \"entry\": \"radio-sx1262.drv.elf\", \"config\": {\"spi_bus\": -1, \"cs\": -1, \"dio1\": -1, \"rst\": -1, \"busy\": -1, \"internal\": true}}\n"
"    ]\n"
"}\n";

/* ── Profiles by board id ─────────────────────────────────────────────────── *
 * The board id repeats what the profile already declares so a match can be
 * logged and asserted by name; validate_board_catalog.py checks the two
 * agree.                                                                     */

static const board_profile_t PROFILE_TDECK      = { "tdeck",       JSON_TDECK      };
static const board_profile_t PROFILE_TDECK_PRO  = { "tdeck-pro",   JSON_TDECK_PRO  };
static const board_profile_t PROFILE_TDECK_MAX  = { "tdeck-max",   JSON_TDECK_MAX  };
static const board_profile_t PROFILE_TDISPLAY   = { "tdisplay-s3", JSON_TDISPLAY   };
static const board_profile_t PROFILE_CARDPUTER  = { "cardputer",   JSON_CARDPUTER  };
static const board_profile_t PROFILE_HELTEC_V3  = { "heltec-v3",   JSON_HELTEC_V3  };
static const board_profile_t PROFILE_T3_S3      = { "t3-s3",       JSON_T3_S3      };
static const board_profile_t PROFILE_RAK3312    = { "rak3312",     JSON_RAK3312    };

/* ── Fingerprint matching ─────────────────────────────────────────────────── *
 * A fingerprint is what answered on a given pin pair, so where boards share a
 * part the rules name their pin pair too.                                     */

/* The T-Deck Pro and the Max it was revised into. */
static bool on_tdeck_pro_i2c(const board_scan_t *scan)
{
    return scan->sda == 13 && scan->scl == 14;
}

/* The T-Deck and the T-Deck Plus. */
static bool on_tdeck_i2c(const board_scan_t *scan)
{
    return scan->sda == 18 && scan->scl == 8;
}

static bool on_heltec_v3_i2c(const board_scan_t *scan)
{
    return scan->sda == 17 && scan->scl == 18;
}

static bool on_t3_s3_i2c(const board_scan_t *scan)
{
    return scan->sda == 18 && scan->scl == 17;
}

bool board_scan_found_anything(const board_scan_t *scan)
{
    return scan->has_tca8418 || scan->has_cst328 || scan->has_bhi260ap ||
           scan->has_cst816  || scan->has_ssd1306 || scan->has_cardkb  ||
           scan->has_xl9555;
}

const board_profile_t *board_fingerprint_match(const board_scan_t *scan)
{
    /* T-Deck Max. The Max is the Pro's third hardware revision and carries the
     * Pro's keyboard controller and IMU at the same addresses on the same pins,
     * so the XL9555 IO expander is the only thing telling them apart — it is
     * absent from every other board in the catalogue, and is the marker LilyGo
     * themselves document for this. Matched before the Pro, whose rule it would
     * otherwise satisfy.
     *
     * Keyed on the expander alone rather than on the expander plus the parts
     * the Pro also has: on the Max those parts sit on rails behind this very
     * expander, and detection runs long before anything has driven it, so they
     * cannot be relied on to answer. */
    if (scan->has_xl9555) {
        return &PROFILE_TDECK_MAX;
    }

    /* T-Deck Pro. A keyboard and an IMU no longer name a Pro on their own: the
     * Max answers to that much too, so the expander's absence is part of the
     * rule rather than left to the order these are tried, and the pin pair is
     * named rather than assumed. A Max whose expander stayed quiet comes up
     * unidentified, which is recoverable; coming up as a Pro is not. */
    if (on_tdeck_pro_i2c(scan) && scan->has_tca8418 && scan->has_bhi260ap &&
        !scan->has_xl9555) {
        return &PROFILE_TDECK_PRO;
    }

    /* T-Deck / T-Deck Plus: the keyboard controller on the T-Deck's own pins,
     * whether or not touch answers. Naming the pin pair is what makes this
     * safe — on the Pro's pins a keyboard alone is a Pro or a Max whose other
     * parts stayed quiet, which is not something to guess at. */
    if (on_tdeck_i2c(scan) && scan->has_tca8418) {
        return &PROFILE_TDECK;
    }

    /* T-Display-S3: CST816 touch, no keyboard */
    if (scan->has_cst816) {
        return &PROFILE_TDISPLAY;
    }

    /* Cardputer: CardKB keyboard */
    if (scan->has_cardkb) {
        return &PROFILE_CARDPUTER;
    }

    /* SSD1306 OLED: Heltec V3 or T3-S3, which carry the same part and are told
     * apart by their pin pair alone. */
    if (scan->has_ssd1306) {
        if (on_heltec_v3_i2c(scan)) {
            return &PROFILE_HELTEC_V3;
        }
        if (on_t3_s3_i2c(scan)) {
            return &PROFILE_T3_S3;
        }
    }

    return NULL;
}

const board_profile_t *board_fingerprint_headless(void)
{
    return &PROFILE_RAK3312;
}

const char *board_fingerprint_profile_json(const char *board_id)
{
    static const board_profile_t *const ALL[] = {
        &PROFILE_TDECK, &PROFILE_TDECK_PRO, &PROFILE_TDECK_MAX, &PROFILE_TDISPLAY,
        &PROFILE_CARDPUTER, &PROFILE_HELTEC_V3, &PROFILE_T3_S3, &PROFILE_RAK3312,
    };
    if (!board_id) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(ALL) / sizeof(ALL[0]); i++) {
        if (strcmp(ALL[i]->board_id, board_id) == 0) {
            return ALL[i]->json;
        }
    }
    return NULL;
}
