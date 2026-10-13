/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the SoC side of sleep, for the power manager
 *
 * Every call into ESP-IDF's sleep API is here, so that power_manager.rs keeps
 * to deciding and the simulators need none of it. Pins are described by the
 * board profile's "power" section and handed in by the caller.
 *
 * Light sleep keeps RAM and resumes where it stopped. Two things about it on
 * the S3 shape this file:
 *
 * - A GPIO wake source is a level, not an edge, and arming one rewrites the
 *   pin's interrupt type. A driver's edge interrupt on that pin would fire
 *   without end once the level is reached, so each wake pin's configuration
 *   is saved, its interrupt is masked through the sleep, and the saved
 *   configuration is put back afterwards. The edge the sleep swallowed is
 *   then replayed for a pin that is still at its level, so its driver hears
 *   about it as it would have awake: the SX1262's receive interrupt is how
 *   the packet that woke the CPU gets read.
 *
 * - CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND isolates every pad during light
 *   sleep (sleep_gpio.c, esp_sleep_startup_init). It guards input-only pins
 *   against a reset from an electrostatic pulse, but it also lets every
 *   output float: the LoRa, e-paper and SD chip selects, and the modem's DTR,
 *   whose rising level is what keeps the modem asleep. So the pads driven as
 *   outputs keep their configuration through the sleep; inputs stay isolated.
 *
 * Deep sleep restarts the SoC on waking. It is woken by EXT1 on any of the
 * given pins going low, which the RTC domain watches; afterwards those pads
 * are still routed to it, and power_platform_release_pins() hands them back.
 */

#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_private/esp_gpio_reserve.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const char *TAG = "power";

/* Mirrors WakePin in power_manager.rs. */
typedef struct {
    int32_t gpio;
    bool active_high;
} power_pin_t;

/* What power_platform_light_sleep() saw on waking. */
typedef struct {
    uint32_t active;   /* bit i: pins[i] was at its active level */
    bool timer;        /* the timer woke it */
} power_wake_t;

#define MAX_PINS 16

bool power_platform_can_wake_deep(int32_t gpio);

static bool valid_pin(int32_t gpio)
{
    return gpio >= 0 && gpio < SOC_GPIO_PIN_COUNT && GPIO_IS_VALID_GPIO(gpio);
}

static bool at_active_level(const power_pin_t *pin)
{
    return gpio_get_level((gpio_num_t)pin->gpio) == (pin->active_high ? 1 : 0);
}

/* A pin's configuration register: its interrupt type and enable among it. */
static uint32_t pin_reg(int32_t gpio)
{
    return GPIO_PIN0_REG + 4 * (uint32_t)gpio;
}

/* Set a pin's interrupt status as if its edge had just been seen. */
static void replay_interrupt(int32_t gpio)
{
    if (gpio < 32) {
        REG_WRITE(GPIO_STATUS_W1TS_REG, 1UL << gpio);
    }
#ifdef GPIO_STATUS1_W1TS_REG
    else {
        REG_WRITE(GPIO_STATUS1_W1TS_REG, 1UL << (gpio - 32));
    }
#endif
}

static bool drives_output(int32_t gpio)
{
    gpio_io_config_t io;
    return gpio_get_io_config((gpio_num_t)gpio, &io) == ESP_OK && io.oe;
}

/* Keep every pad driven as an output, other than the flash's and PSRAM's,
 * which IDF's own sleep workarounds look after, driven through the sleep.
 * Returns the pins changed, for keep_outputs_end(). */
static uint64_t keep_outputs_begin(uint64_t skip)
{
    uint64_t kept = 0;
    for (int32_t gpio = 0; gpio < SOC_GPIO_PIN_COUNT; gpio++) {
        uint64_t bit = 1ULL << gpio;
        if ((skip & bit) || !GPIO_IS_VALID_GPIO(gpio) || esp_gpio_is_reserved(bit)
            || !drives_output(gpio)) {
            continue;
        }
        if (gpio_sleep_sel_dis((gpio_num_t)gpio) == ESP_OK) {
            kept |= bit;
        }
    }
    return kept;
}

static void keep_outputs_end(uint64_t kept)
{
    for (int32_t gpio = 0; gpio < SOC_GPIO_PIN_COUNT; gpio++) {
        if (kept & (1ULL << gpio)) {
            gpio_sleep_sel_en((gpio_num_t)gpio);
        }
    }
}

esp_err_t power_platform_light_sleep(const power_pin_t *pins, size_t count,
                                     uint64_t timeout_us, power_wake_t *out)
{
    if (!out || count > MAX_PINS || (count && !pins)) {
        return ESP_ERR_INVALID_ARG;
    }
    out->active = 0;
    out->timer = false;

    uint32_t saved[MAX_PINS];
    uint64_t wake_mask = 0;
    for (size_t i = 0; i < count; i++) {
        if (!valid_pin(pins[i].gpio)) {
            return ESP_ERR_INVALID_ARG;
        }
        saved[i] = REG_READ(pin_reg(pins[i].gpio));
        wake_mask |= 1ULL << pins[i].gpio;
    }

    for (size_t i = 0; i < count; i++) {
        gpio_num_t gpio = (gpio_num_t)pins[i].gpio;
        gpio_intr_disable(gpio);
        gpio_wakeup_enable(gpio, pins[i].active_high ? GPIO_INTR_HIGH_LEVEL
                                                     : GPIO_INTR_LOW_LEVEL);
    }
    if (count) {
        esp_sleep_enable_gpio_wakeup();
    }
    if (timeout_us) {
        esp_sleep_enable_timer_wakeup(timeout_us);
    }
    uint64_t kept = keep_outputs_begin(wake_mask);

    esp_err_t ret = esp_light_sleep_start();

    keep_outputs_end(kept);
    out->timer = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    for (size_t i = 0; i < count; i++) {
        gpio_num_t gpio = (gpio_num_t)pins[i].gpio;
        bool active = at_active_level(&pins[i]);
        if (active) {
            out->active |= 1UL << i;
        }
        gpio_wakeup_disable(gpio);
        REG_WRITE(pin_reg(gpio), saved[i]);
        bool had_interrupt = (saved[i] >> GPIO_PIN0_INT_ENA_S) & GPIO_PIN0_INT_ENA_V;
        if (active && had_interrupt) {
            replay_interrupt(gpio);
        }
    }
    return ret;
}

esp_err_t power_platform_deep_sleep(const power_pin_t *pins, size_t count)
{
    if (count > MAX_PINS || (count && !pins)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t mask = 0;
    for (size_t i = 0; i < count; i++) {
        /* One level for all the pins, and only pins the sleeping chip watches. */
        if (!power_platform_can_wake_deep(pins[i].gpio) || pins[i].active_high) {
            return ESP_ERR_INVALID_ARG;
        }
        mask |= 1ULL << pins[i].gpio;
    }
    if (mask) {
#if SOC_PM_SUPPORT_EXT1_WAKEUP
        /* The original ESP32 has no "any low" EXT1; it refuses this. */
        esp_err_t ret = esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
        esp_err_t ret = esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
#else
        esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
#endif
        if (ret != ESP_OK) {
            return ret;
        }
    }
    /* Pads a driver holds, such as the modem's DTR, keep their level. */
    gpio_deep_sleep_hold_en();
    ESP_LOGI(TAG, "deep sleep (wake mask 0x%llx)", (unsigned long long)mask);
    esp_deep_sleep_start();
    return ESP_FAIL;  // not reached
}

uint32_t power_platform_boot_wake(const power_pin_t *pins, size_t count)
{
    uint64_t status = 0;
    switch (esp_sleep_get_wakeup_cause()) {
#if SOC_PM_SUPPORT_EXT1_WAKEUP
    case ESP_SLEEP_WAKEUP_EXT1:
        status = esp_sleep_get_ext1_wakeup_status();
        break;
#endif
#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    case ESP_SLEEP_WAKEUP_GPIO:
        status = esp_sleep_get_gpio_wakeup_status();
        break;
#endif
    default:
        return 0;
    }
    uint32_t woke = 0;
    for (size_t i = 0; i < count && i < 32; i++) {
        if (valid_pin(pins[i].gpio) && (status & (1ULL << pins[i].gpio))) {
            woke |= 1UL << i;
        }
    }
    return woke;
}

bool power_platform_woke_from_deep_sleep(void)
{
    return esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED;
}

void power_platform_release_pins(const power_pin_t *pins, size_t count)
{
#if SOC_RTCIO_PIN_COUNT > 0
    for (size_t i = 0; i < count; i++) {
        gpio_num_t gpio = (gpio_num_t)pins[i].gpio;
        if (valid_pin(pins[i].gpio) && rtc_gpio_is_valid_gpio(gpio)) {
#if SOC_RTCIO_HOLD_SUPPORTED
            rtc_gpio_hold_dis(gpio);
#endif
            rtc_gpio_deinit(gpio);
        }
    }
#else
    (void)pins;
    (void)count;
#endif
}

bool power_platform_can_wake_deep(int32_t gpio)
{
    return valid_pin(gpio) && esp_sleep_is_valid_wakeup_gpio((gpio_num_t)gpio);
}

/* An input nobody else configures: the power key, or the modem's RI, which
 * floats while the modem's level shifter is off. */
esp_err_t power_platform_setup_input(int32_t gpio, bool active_high)
{
    if (!valid_pin(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = active_high ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

bool power_platform_pin_active(int32_t gpio, bool active_high)
{
    if (!valid_pin(gpio)) {
        return false;
    }
    power_pin_t pin = {.gpio = gpio, .active_high = active_high};
    return at_active_level(&pin);
}

void power_platform_restart(void)
{
    esp_restart();
}
