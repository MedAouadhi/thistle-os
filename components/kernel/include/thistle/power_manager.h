/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — sleep, deep sleep and the power key
 *
 * The device sleeps once nobody has pressed a key or touched the screen for
 * a while, and goes into deep sleep after a longer while. Both times are set
 * in Settings and kept in system.json under thistle_os.power.
 *
 * Sleep halts the CPU and keeps RAM. The screen says the device is asleep,
 * WiFi stops, the modem sleeps still listening for calls and SMS, and LoRa
 * keeps receiving. The power key, a key on the keyboard, the modem's RI line
 * and a LoRa packet wake it; only the first two bring the screen back for
 * good, the others let the system handle what came in and sleep again.
 *
 * Deep sleep powers down nearly everything, and waking from it restarts the
 * device. The power key and the keyboard wake it, and so does the modem's RI
 * line while "wake on calls and SMS" is on (the modem then stays asleep but
 * powered, about 2 mA against the board's 0.34 mA with it off).
 *
 * Which pins wake the device, and which rails deep sleep turns off, come from
 * the "power" section of the board profile. A board without one never sleeps.
 * Implemented in components/kernel_rs/src/power_manager.rs.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sleep_after_s;       /* 0: never */
    uint32_t deep_sleep_after_s;  /* 0: never */
    bool wake_on_modem;           /* keep the modem listening in deep sleep */
    bool stay_awake_on_usb;       /* no sleeping by the clock while on USB power */
} power_settings_t;

typedef struct {
    bool has_key;            /* the board has a power key */
    bool can_sleep;          /* something other than a timer can wake it */
    bool can_deep_sleep;     /* ...and can wake it from deep sleep */
    bool can_wake_on_modem;  /* the modem's RI line can wake it */
} power_caps_t;

typedef enum {
    POWER_REQUEST_SLEEP,
    POWER_REQUEST_DEEP_SLEEP,
    POWER_REQUEST_RESTART,
} power_request_t;

/* Start watching for idle time and the power key. app_main() calls this once
 * the boot has settled, after ota_mark_valid(): waking from deep sleep is a
 * restart, and one before the image is marked valid would roll it back. */
void power_manager_start(void);

esp_err_t power_manager_get_settings(power_settings_t *out);
void power_manager_get_caps(power_caps_t *out);

/* Each saves to system.json and takes effect at once. */
esp_err_t power_manager_set_sleep_after(uint32_t seconds);
esp_err_t power_manager_set_deep_sleep_after(uint32_t seconds);
esp_err_t power_manager_set_wake_on_modem(bool on);
esp_err_t power_manager_set_stay_awake_on_usb(bool on);

/* Sleep, deep sleep or restart now. Returns at once; the power manager acts
 * on it from its own job. ESP_ERR_NOT_SUPPORTED if the board cannot. */
esp_err_t power_manager_request(power_request_t request);

/* Someone used the device. Window managers call this for every input event;
 * it only stores a time, so any task may. */
void power_manager_note_activity(void);

/* Keep the device awake while something runs that sleep would break: an
 * update being written, audio playing, a download. `why` says what, as the
 * end of "Not sleeping: ..." ("audio is playing"), and must outlive the hold
 * (a string literal). Returns a token for
 * power_manager_release(), never 0. Neither call blocks, and both may be
 * made from any task, but not from an interrupt. */
uint32_t power_manager_hold(const char *why);
void power_manager_release(uint32_t token);

/* What keeps the device awake, in words ("audio is playing"), or NULL. The
 * `why` of the first hold, else a call ringing, else Bluetooth being on. */
const char *power_manager_held_by(void);

/* ── For window managers ─────────────────────────────────────────────────
 *
 * Before the device sleeps, every task that draws or reads input stops where
 * it is safe to, so that sleep never lands in the middle of a bus transfer.
 * Each such task calls power_ui_join() once when it starts, then
 * power_ui_checkpoint() at the top of every pass of its loop. The checkpoint
 * returns at once while the device is awake. When sleep is coming it blocks,
 * from the first pass where `ready` is true, until the device wakes.
 * `ready` is false while the task still has something to put on the screen,
 * such as the sleep screen. */
void power_ui_join(void);
void power_ui_checkpoint(bool ready);

typedef enum {
    POWER_UI_NONE,
    POWER_UI_SHOW_SLEEP_SCREEN,
    POWER_UI_HIDE_SLEEP_SCREEN,
    POWER_UI_SHOW_MENU,
    POWER_UI_SHOW_HELD_OFF,   /* sleep was asked for; say what holds it off */
} power_ui_request_t;

/* The next thing the power manager wants shown, once. Polled by a window
 * manager that can show it. */
power_ui_request_t power_ui_take_request(void);

/* A window manager that can show the power menu says so; without one, holding
 * the power key goes straight to deep sleep. */
void power_ui_set_menu_available(bool available);

#ifdef __cplusplus
}
#endif
