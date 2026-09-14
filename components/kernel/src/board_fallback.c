/*
 * I2C-based board auto-detection fallback.
 *
 * When no board profile is present on SPIFFS, this code probes known I2C
 * addresses across common pin pairs to fingerprint the attached hardware.  On
 * a match it writes that board's embedded fallback profile to SPIFFS so the
 * Rust board_config_init() can load it on retry.  The profiles, and the rules
 * that choose between them, live in board_fingerprint.c.
 *
 * Declared __attribute__((weak)) so that an explicitly linked board component
 * (board_tdeck, board_tdeck_pro, etc.) can override it with a strong symbol.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "hal/board.h"
#include "thistle/board_fingerprint.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

/* ESP-IDF I2C master driver — used only on real hardware */
#if !defined(THISTLE_SIMULATOR)
#include "driver/i2c_master.h"
#endif

static const char *TAG = "board_detect";

/* ── I2C pin combinations to try ──────────────────────────────────────────── */

typedef struct {
    int sda;
    int scl;
} i2c_pins_t;

/* Ordered by popularity: most ESP32-S3 boards use one of these */
static const i2c_pins_t I2C_PIN_COMBOS[] = {
    { 18,  8 },   /* T-Deck, T-Deck Plus */
    { 18, 17 },   /* T-Display-S3, T3-S3 */
    { 13, 14 },   /* T-Deck Pro, T-Deck Max */
    { 17, 18 },   /* Heltec V3 */
    {  1,  2 },   /* Cardputer */
    {  4,  5 },   /* RAK3312 */
};
#define NUM_PIN_COMBOS (sizeof(I2C_PIN_COMBOS) / sizeof(I2C_PIN_COMBOS[0]))

/* ── I2C probe helpers (real hardware only) ───────────────────────────────── */

#if !defined(THISTLE_SIMULATOR)

/* Probe a single I2C address — returns true if device ACKs.
 * Uses i2c_master_probe() (added in IDF v5.1, required in v6 since
 * zero-length i2c_master_transmit is no longer allowed). */
static bool i2c_probe(i2c_master_bus_handle_t bus, uint8_t addr)
{
    return (i2c_master_probe(bus, addr, 50) == ESP_OK);
}

/* Try one pin combo: init bus, probe all known addresses, tear down */
static bool try_pin_combo(const i2c_pins_t *pins, board_scan_t *out)
{
    memset(out, 0, sizeof(*out));
    out->sda = pins->sda;
    out->scl = pins->scl;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = pins->sda,
        .scl_io_num = pins->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        return false;
    }

    out->has_tca8418  = i2c_probe(bus, BOARD_ADDR_TCA8418);
    out->has_cst328   = i2c_probe(bus, BOARD_ADDR_CST328);
    out->has_bhi260ap = i2c_probe(bus, BOARD_ADDR_BHI260AP);
    out->has_cst816   = i2c_probe(bus, BOARD_ADDR_CST816);
    out->has_ssd1306  = i2c_probe(bus, BOARD_ADDR_SSD1306);
    out->has_cardkb   = i2c_probe(bus, BOARD_ADDR_CARDKB);
    out->has_xl9555   = i2c_probe(bus, BOARD_ADDR_XL9555);

    i2c_del_master_bus(bus);

    return board_scan_found_anything(out);
}
#endif /* !THISTLE_SIMULATOR */

/* ── Write the board profile to SPIFFS ────────────────────────────────────── */

static esp_err_t write_board_profile(const char *json_content)
{
    /* Ensure config directory exists */
    mkdir("/spiffs/config", 0755);

    FILE *f = fopen("/spiffs/config/board.json", "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create /spiffs/config/board.json");
        return ESP_FAIL;
    }
    fputs(json_content, f);
    fclose(f);
    ESP_LOGI(TAG, "Wrote the detected board's profile to /spiffs/config/board.json");
    return ESP_OK;
}

/* ── Public API: detect board and write config ────────────────────────────── */

esp_err_t board_detect_and_write(void)
{
#if defined(THISTLE_SIMULATOR)
    ESP_LOGW(TAG, "Board detection not available in simulator");
    return ESP_ERR_NOT_SUPPORTED;
#else
    ESP_LOGI(TAG, "Auto-detecting board via I2C scan...");

    const board_profile_t *matched = NULL;
    bool any_device_anywhere = false;

    /* Try each known pin combination until we get a match */
    for (int i = 0; i < (int)NUM_PIN_COMBOS; i++) {
        ESP_LOGI(TAG, "Trying I2C pins SDA=%d SCL=%d...",
                 I2C_PIN_COMBOS[i].sda, I2C_PIN_COMBOS[i].scl);

        board_scan_t result;
        bool any_device = try_pin_combo(&I2C_PIN_COMBOS[i], &result);

        if (any_device) {
            any_device_anywhere = true;
            ESP_LOGI(TAG, "I2C scan (SDA=%d SCL=%d): TCA8418=%d CST328=%d "
                     "BHI260=%d CST816=%d SSD1306=%d CardKB=%d XL9555=%d",
                     result.sda, result.scl,
                     result.has_tca8418, result.has_cst328, result.has_bhi260ap,
                     result.has_cst816, result.has_ssd1306, result.has_cardkb,
                     result.has_xl9555);

            matched = board_fingerprint_match(&result);
            if (matched) {
                ESP_LOGI(TAG, "Detected: %s (SDA=%d SCL=%d)",
                         matched->board_id, result.sda, result.scl);
                break;
            }
        }
    }

    if (!matched && !any_device_anywhere) {
        /* No I2C devices found on any pin combo — headless board */
        matched = board_fingerprint_headless();
        ESP_LOGI(TAG, "No I2C devices found on any pin combo — assuming headless (%s)",
                 matched->board_id);
    }

    if (!matched) {
        /* Devices answered but no rule claimed them.  Writing a guess here is
         * worse than writing nothing: the board would come up confidently as
         * something it is not, with no error to go on. */
        ESP_LOGW(TAG, "I2C devices found but no board matched — leaving unidentified");
        return ESP_ERR_NOT_FOUND;
    }

    return write_board_profile(matched->json);
#endif
}

/* ── Legacy fallback — still weak so compiled board components override ──── */

__attribute__((weak))
esp_err_t board_init(void)
{
    ESP_LOGW(TAG, "No board.json found — running in safe mode");
    ESP_LOGW(TAG, "Insert SD card with config/boards/<board>.json and reboot");
    ESP_LOGW(TAG, "Or use Recovery mode to provision the device");

    hal_set_board_name("Unknown (safe mode)");

    return ESP_OK;
}
