// SPDX-License-Identifier: BSD-3-Clause
// Board builtin driver initialization — called from board_config.rs when
// ELF driver files are not present on SPIFFS/SD. Provides bus init helpers
// and maps driver IDs to compiled-in vtables.

#ifdef THISTLE_BUS_INIT_HOST_TEST
#include "board_bus_init_test_shim.h"
#else
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/board.h"
#include "drv_epaper_gdeq031t10.h"
#include "drv_kbd_tca8418.h"
#include "drv_touch_cst328.h"
#include "drv_touch_cst66xx.h"
#include "drv_sdcard.h"
#include "drv_power_sy6970.h"
#include "drv_gps_mia_m10q.h"
#include "drv_radio_sx1262.h"
#include "drv_audio_es8311.h"
#include "drv_imu_bhi260ap.h"
#endif

#ifndef THISTLE_BUS_INIT_HOST_TEST
static const char *TAG = "board_builtin";
#endif

// ---------------------------------------------------------------------------
// Minimal JSON value readers, over a driver entry's config sub-object.
// Each finds "key": and then reads the value in the shape it expects.
// ---------------------------------------------------------------------------

#ifndef THISTLE_BUS_INIT_HOST_TEST
// The cursor just past `"key":` and any whitespace, or NULL when the key is
// absent. The readers below differ only in what they make of what follows.
static const char *json_value(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    char pattern[80];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    return p;
}

// Handles bare integers and quoted hex ("0x34").
static int json_int(const char *json, const char *key, int default_val)
{
    const char *p = json_value(json, key);
    if (!p) return default_val;
    // Quoted value — may be hex ("0x34") or decimal string
    if (*p == '"') p++;
    return (int)strtol(p, NULL, 0);
}

// Copy a quoted JSON string value into `out`. Returns false and leaves `out`
// untouched when the key is missing or its value is not a string.
static bool json_str(const char *json, const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    const char *p = json_value(json, key);
    if (!p || *p != '"') return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len >= out_size) return false;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

// Board profiles write flags as `true`/`false`, which json_int reads as zero
// either way — so a flag left to json_int is a flag that is always off.
static bool json_bool(const char *json, const char *key, bool default_val)
{
    const char *p = json_value(json, key);
    if (!p) return default_val;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return default_val;
}
#endif

// ---------------------------------------------------------------------------
// Bus initialisation helpers (called from board_config.rs via FFI)
// ---------------------------------------------------------------------------

esp_err_t board_bus_init_spi(int host, int mosi, int miso, int sclk, int max_transfer_bytes)
{
    spi_bus_config_t cfg = {
        .mosi_io_num     = mosi,
        .miso_io_num     = miso,
        .sclk_io_num     = sclk,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = max_transfer_bytes > 0 ? max_transfer_bytes : 4096,
    };
    esp_err_t ret = spi_bus_initialize((spi_host_device_t)host, &cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize host=%d failed: %s", host, esp_err_to_name(ret));
        return ret;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI host %d already initialized", host);
    }
    // Store host ID as the "handle" — ELF drivers call hal_bus_get_spi(idx)
    // and cast it back to spi_host_device_t.
    esp_err_t register_ret = hal_bus_register_spi(host, (void *)(intptr_t)host);
    if (register_ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI host %d registration failed: %s",
                 host, esp_err_to_name(register_ret));
        if (ret == ESP_OK) {
            esp_err_t cleanup_ret = spi_bus_free((spi_host_device_t)host);
            if (cleanup_ret != ESP_OK) {
                ESP_LOGE(TAG, "SPI host %d cleanup failed: %s",
                         host, esp_err_to_name(cleanup_ret));
            }
        }
        return register_ret;
    }
    ESP_LOGI(TAG, "SPI host %d ready (MOSI=%d MISO=%d SCLK=%d)", host, mosi, miso, sclk);
    return ESP_OK;
}

esp_err_t board_bus_init_i2c(int port, int sda, int scl, int freq_hz)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port              = (i2c_port_t)port,
        .sda_io_num            = (gpio_num_t)sda,
        .scl_io_num            = (gpio_num_t)scl,
        .clk_source            = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt     = 7,
        .flags.enable_internal_pullup = true,
    };
    (void)freq_hz; // freq_hz is set per-device in i2c_master_probe/add_device
    i2c_master_bus_handle_t handle;
    esp_err_t ret = i2c_new_master_bus(&cfg, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus port=%d failed: %s", port, esp_err_to_name(ret));
        return ret;
    }
    esp_err_t register_ret = hal_bus_register_i2c(port, handle);
    if (register_ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C port %d registration failed: %s",
                 port, esp_err_to_name(register_ret));
        esp_err_t cleanup_ret = i2c_del_master_bus(handle);
        if (cleanup_ret != ESP_OK) {
            ESP_LOGE(TAG, "I2C port %d cleanup failed: %s",
                     port, esp_err_to_name(cleanup_ret));
        }
        return register_ret;
    }
    ESP_LOGI(TAG, "I2C port %d ready (SDA=%d SCL=%d)", port, sda, scl);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Builtin driver init — called when a .drv.elf is absent from SPIFFS/SD.
// id and hal_type come from board.json; config_json is the per-driver config
// sub-object.
// ---------------------------------------------------------------------------

#ifndef THISTLE_BUS_INIT_HOST_TEST
esp_err_t board_builtin_driver_init(const char *id, const char *hal_type,
                                    const char *config_json)
{
    if (!id || !hal_type || !config_json) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "builtin: %s (hal=%s)", id, hal_type);

    // ── E-paper display: GDEQ031T10 ─────────────────────────────────────────
    if (strcmp(id, "com.thistle.drv.epaper-gdeq031t10") == 0) {
        static epaper_gdeq031t10_config_t cfg;
        int spi_bus_idx = json_int(config_json, "spi_bus", 0);
        // hal_bus_get_spi stores the host ID as the pointer value
        cfg.spi_host    = (spi_host_device_t)(intptr_t)hal_bus_get_spi(spi_bus_idx);
        cfg.pin_cs      = (gpio_num_t)json_int(config_json, "cs",           -1);
        cfg.pin_dc      = (gpio_num_t)json_int(config_json, "dc",           -1);
        cfg.pin_rst     = (gpio_num_t)json_int(config_json, "rst",          -1);
        cfg.pin_busy    = (gpio_num_t)json_int(config_json, "busy",         -1);
        cfg.pin_bl      = (gpio_num_t)json_int(config_json, "bl",           -1);
        cfg.spi_clock_hz =            json_int(config_json, "spi_clock_hz", 2000000);
        cfg.rotation     =            json_int(config_json, "rotation",     0);
        ESP_LOGI(TAG, "epaper: host=%d cs=%d dc=%d rst=%d busy=%d bl=%d clk=%d rot=%d",
                 cfg.spi_host, cfg.pin_cs, cfg.pin_dc, cfg.pin_rst,
                 cfg.pin_busy, cfg.pin_bl, cfg.spi_clock_hz, cfg.rotation);
        return hal_display_register(drv_epaper_gdeq031t10_get(), &cfg);
    }

    // ── Keyboard: TCA8418 ────────────────────────────────────────────────────
    if (strcmp(id, "com.thistle.drv.kbd-tca8418") == 0) {
        static kbd_tca8418_config_t cfg;
        int i2c_bus_idx = json_int(config_json, "i2c_bus", 0);
        cfg.i2c_bus  = (i2c_master_bus_handle_t)hal_bus_get_i2c(i2c_bus_idx);
        cfg.i2c_addr = (uint8_t)json_int(config_json, "i2c_addr", 0x34);
        cfg.pin_int  = (gpio_num_t)json_int(config_json, "pin_int", -1);
        cfg.pin_backlight = (gpio_num_t)json_int(config_json, "pin_backlight", -1);
        ESP_LOGI(TAG, "kbd-tca8418: i2c_bus=%p addr=0x%02x int=%d backlight=%d",
                 (void *)cfg.i2c_bus, cfg.i2c_addr, cfg.pin_int, cfg.pin_backlight);
        return hal_input_register(drv_kbd_tca8418_get(), &cfg);
    }

    // ── Touchscreen: CST328 ──────────────────────────────────────────────────
    if (strcmp(id, "com.thistle.drv.touch-cst328") == 0) {
        static touch_cst328_config_t cfg;
        int i2c_bus_idx = json_int(config_json, "i2c_bus", 0);
        cfg.i2c_bus  = (i2c_master_bus_handle_t)hal_bus_get_i2c(i2c_bus_idx);
        cfg.i2c_addr = (uint8_t)json_int(config_json, "i2c_addr", 0x1A);
        cfg.pin_int  = (gpio_num_t)json_int(config_json, "pin_int", -1);
        cfg.pin_rst  = (gpio_num_t)json_int(config_json, "pin_rst", -1);
        cfg.max_x    = (uint16_t)json_int(config_json, "max_x", 240);
        cfg.max_y    = (uint16_t)json_int(config_json, "max_y", 320);
        ESP_LOGI(TAG, "touch-cst328: i2c_bus=%p addr=0x%02x int=%d rst=%d",
                 (void *)cfg.i2c_bus, cfg.i2c_addr, cfg.pin_int, cfg.pin_rst);
        return hal_input_register(drv_touch_cst328_get(), &cfg);
    }

    // ── Touch: Hynitron CST66xx ──────────────────────────────────────────────
    // The T-Deck Max's part, despite the vendor pinmap calling it a CST328 and
    // the vendor README calling it a CST3530. Its own firmware identifies it
    // as `ictype:caca220e`, which is the CST66xx probe signature.
    if (strcmp(id, "com.thistle.drv.touch-cst66xx") == 0) {
        static touch_cst66xx_config_t cfg;
        int i2c_bus_idx = json_int(config_json, "i2c_bus", 0);
        cfg.i2c_bus  = (i2c_master_bus_handle_t)hal_bus_get_i2c(i2c_bus_idx);
        cfg.i2c_addr = (uint8_t)json_int(config_json, "i2c_addr", 0x1A);
        cfg.pin_int  = (gpio_num_t)json_int(config_json, "pin_int", -1);
        cfg.pin_rst  = (gpio_num_t)json_int(config_json, "pin_rst", -1);
        // On this board the touch reset is an XL9555 output rather than a
        // GPIO, and the driver has to pulse it itself before it probes.
        cfg.rst_expander     = json_int(config_json, "rst_expander", -1);
        cfg.rst_expander_pin = json_int(config_json, "rst_expander_pin", 0);
        cfg.max_x    = (uint16_t)json_int(config_json, "max_x", 240);
        cfg.max_y    = (uint16_t)json_int(config_json, "max_y", 320);
        // The heart key, leftmost of the three below the T-Deck Max's panel,
        // is key 0: read off the board, since the chip only numbers its keys
        // and the vendor repo names none of them. The default serves a board
        // whose stored profile predates "home_key".
        cfg.home_key = json_int(config_json, "home_key", 0);
        ESP_LOGI(TAG, "touch-cst66xx: i2c_bus=%p addr=0x%02x int=%d rst_exp=%d/%d home_key=%d",
                 (void *)cfg.i2c_bus, cfg.i2c_addr, cfg.pin_int,
                 (int)cfg.rst_expander, (int)cfg.rst_expander_pin, (int)cfg.home_key);
        return hal_input_register(drv_touch_cst66xx_get(), &cfg);
    }

    // ── SD card ──────────────────────────────────────────────────────────────
    // Without this a board that boots from a board profile never registers
    // storage at all: the compiled-in board initialisers are the only other
    // place the SD card is registered, and they do not run once a profile
    // exists.
    if (strcmp(id, "com.thistle.drv.sdcard") == 0) {
        static sdcard_config_t cfg;
        static char mount_point[32];
        int spi_bus_idx  = json_int(config_json, "spi_bus", 0);
        // hal_bus_get_spi stores the host ID as the pointer value
        cfg.spi_host     = (spi_host_device_t)(intptr_t)hal_bus_get_spi(spi_bus_idx);
        cfg.pin_cs       = (gpio_num_t)json_int(config_json, "pin_cs", -1);
        cfg.max_files    = json_int(config_json, "max_files", 5);
        cfg.mount_point  = json_str(config_json, "mount_point", mount_point, sizeof(mount_point))
                               ? mount_point
                               : "/sdcard";
        ESP_LOGI(TAG, "sdcard: host=%d cs=%d mount=%s",
                 cfg.spi_host, cfg.pin_cs, cfg.mount_point);
        return hal_storage_register(drv_sdcard_get(), &cfg);
    }

    // ── GNSS: U-blox MIA-M10Q ────────────────────────────────────────────────
    // `tx` and `rx` are the SoC's pins, not the module's: the schematics on
    // these boards name their nets from the module's side, so the two are
    // crossed over relative to each other.
    if (strcmp(id, "com.thistle.drv.gps-mia-m10q") == 0) {
        static gps_mia_m10q_config_t cfg;
        cfg.uart_num  = (uart_port_t)json_int(config_json, "uart_num", 1);
        cfg.pin_tx    = (gpio_num_t)json_int(config_json, "tx", -1);
        cfg.pin_rx    = (gpio_num_t)json_int(config_json, "rx", -1);
        cfg.baud_rate = (uint32_t)json_int(config_json, "baud_rate", 9600);
        ESP_LOGI(TAG, "gps-mia-m10q: uart=%d tx=%d rx=%d baud=%u",
                 cfg.uart_num, cfg.pin_tx, cfg.pin_rx, (unsigned)cfg.baud_rate);
        return hal_gps_register(drv_gps_mia_m10q_get(), &cfg);
    }

    // ── LoRa: SX1262 ─────────────────────────────────────────────────────────
    // The module shares the board's SPI bus; only its four control lines and
    // the band the board is matched for are its own. A profile that names no
    // band leaves freq_hz zero, which the driver reads as its own 915 MHz
    // default — the boards already in the catalogue are unmoved by this entry.
    if (strcmp(id, "com.thistle.drv.radio-sx1262") == 0) {
        static radio_sx1262_config_t cfg;
        int spi_bus_idx = json_int(config_json, "spi_bus", 0);
        // hal_bus_get_spi stores the host ID as the pointer value
        cfg.spi_host     = (spi_host_device_t)(intptr_t)hal_bus_get_spi(spi_bus_idx);
        cfg.pin_cs       = (gpio_num_t)json_int(config_json, "cs",    -1);
        cfg.pin_reset    = (gpio_num_t)json_int(config_json, "rst",   -1);
        cfg.pin_busy     = (gpio_num_t)json_int(config_json, "busy",  -1);
        cfg.pin_dio1     = (gpio_num_t)json_int(config_json, "dio1",  -1);
        cfg.spi_clock_hz =            json_int(config_json, "spi_clock_hz", 2000000);
        cfg.freq_hz      = (uint32_t) json_int(config_json, "freq_hz", 0);
        // Properties of the module, not the board, but only a board profile
        // knows which module is soldered to it.
        cfg.tcxo_mv        = (uint16_t)json_int(config_json, "tcxo_mv", 0);
        cfg.dio2_rf_switch = json_bool(config_json, "dio2_rf_switch", false);
        ESP_LOGI(TAG, "radio-sx1262: host=%d cs=%d rst=%d busy=%d dio1=%d freq=%uHz "
                      "tcxo=%umV dio2_rf_switch=%d",
                 cfg.spi_host, cfg.pin_cs, cfg.pin_reset, cfg.pin_busy,
                 cfg.pin_dio1, (unsigned)cfg.freq_hz, (unsigned)cfg.tcxo_mv,
                 (int)cfg.dio2_rf_switch);
        return hal_radio_register(drv_radio_sx1262_get(), &cfg);
    }

    // ── Power: SY6970 / BQ25896 charger + BQ27220 fuel gauge ────────────────
    // The driver finds its charger by probing, so the entry says only which
    // bus the two chips sit on.
    if (strcmp(id, "com.thistle.drv.power-sy6970") == 0) {
        ESP_LOGI(TAG, "power-sy6970: %s", config_json);
        return drv_power_sy6970_register(config_json);
    }

    // ── Audio: ES8311 codec ─────────────────────────────────────────────────
    // `i2s_data` is the SoC's data out, which lands on the codec's DSDIN. The
    // amplifier enable is named separately because on the T-Deck Max it is an
    // IO expander pin the driver gates around playback, not a static rail.
    if (strcmp(id, "com.thistle.drv.audio-es8311") == 0) {
        static audio_es8311_config_t cfg;
        int i2c_bus_idx = json_int(config_json, "i2c_bus", 0);
        cfg.i2c_bus          = hal_bus_get_i2c(i2c_bus_idx);
        cfg.i2c_addr         = (uint8_t)json_int(config_json, "i2c_addr", 0x18);
        cfg.i2s_num          = json_int(config_json, "i2s_num",  0);
        cfg.pin_mclk         = json_int(config_json, "i2s_mclk", -1);
        cfg.pin_bclk         = json_int(config_json, "i2s_bck",  -1);
        cfg.pin_ws           = json_int(config_json, "i2s_ws",   -1);
        cfg.pin_dout         = json_int(config_json, "i2s_data", -1);
        cfg.amp_expander     = json_int(config_json, "amp_expander", -1);
        cfg.amp_expander_pin = json_int(config_json, "amp_expander_pin", 0);
        ESP_LOGI(TAG, "audio-es8311: i2c_bus=%p addr=0x%02x i2s=%d mclk=%d bck=%d ws=%d "
                      "data=%d amp_exp=%d/%d",
                 cfg.i2c_bus, cfg.i2c_addr, cfg.i2s_num, cfg.pin_mclk, cfg.pin_bclk,
                 cfg.pin_ws, cfg.pin_dout, (int)cfg.amp_expander, (int)cfg.amp_expander_pin);
        return hal_audio_register(drv_audio_es8311_get(), &cfg);
    }

    // ── IMU: Bosch BHI260AP ─────────────────────────────────────────────────
    // A sensor hub that boots from firmware the driver uploads at init. The
    // entry's `pin_int` is not read: the driver polls its FIFO instead.
    if (strcmp(id, "com.thistle.drv.imu-bhi260ap") == 0) {
        ESP_LOGI(TAG, "imu-bhi260ap: %s", config_json);
        return drv_imu_bhi260ap_register(config_json);
    }

    ESP_LOGW(TAG, "no builtin for driver id=%s — skipping", id);
    return ESP_OK;
}
#endif

// ---------------------------------------------------------------------------
// Settling delay — called from board_config.rs to honour the delay a board
// profile declares on an expander pin, once the rail behind it is raised.
// ---------------------------------------------------------------------------

#ifndef THISTLE_BUS_INIT_HOST_TEST
void board_delay_ms(int ms)
{
    if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
}
#endif

// ---------------------------------------------------------------------------
// GPIO pre-init helper — called from board_config.rs to configure power-enable
// and other board-level GPIOs before driver initialization.
// ---------------------------------------------------------------------------

#ifndef THISTLE_BUS_INIT_HOST_TEST
esp_err_t board_gpio_set_output(int pin, int level, int delay_ms)
{
    if (pin < 0) return ESP_OK;
    gpio_reset_pin((gpio_num_t)pin);
    esp_err_t ret = gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    if (ret != ESP_OK) return ret;
    ret = gpio_set_level((gpio_num_t)pin, (uint32_t)level);
    ESP_LOGI(TAG, "gpio_output: pin=%d level=%d delay=%d ret=%s",
             pin, level, delay_ms, esp_err_to_name(ret));
    if (delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        // After 1V8_EN is toggled, check the e-paper BUSY pin (GPIO 37)
        // to verify the display is wired. When 1V8 is OFF, BUSY should
        // float HIGH (pull-up); when ON, display drives BUSY LOW (idle).
        if (pin == 38) {
            /* UC8253 BUSY: LOW = busy, HIGH = ready. With 1V8 off, the
             * unpowered chip can't drive BUSY so it floats; with 1V8 on,
             * the chip should release BUSY HIGH once POR is complete. */
            gpio_config_t busy_conf = {
                .pin_bit_mask = (1ULL << 37),
                .mode         = GPIO_MODE_INPUT,
                .pull_up_en   = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type    = GPIO_INTR_DISABLE,
            };
            gpio_config(&busy_conf);
            int busy = gpio_get_level((gpio_num_t)37);
            ESP_LOGI(TAG, "1V8_EN=%d -> BUSY(GPIO37)=%d (1=ready, 0=busy/unpowered)",
                     level, busy);
        }
    }
    return ret;
}
#endif
