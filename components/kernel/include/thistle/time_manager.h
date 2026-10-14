/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the system clock and the time zone
 *
 * The kernel keeps the system clock in UTC and sets it from the best source
 * the device has: a time server (NTP) over WiFi or LTE, the GPS receiver
 * while something has it on, the time the mobile network gives the modem,
 * and a battery-backed clock chip on a board that has one. A worse source
 * corrects the clock only when nothing better has confirmed it for an hour.
 * Without a clock chip the time survives a restart and deep sleep, but not
 * a power-on.
 *
 * The time zone is applied to the C library (TZ), so localtime() gives local
 * time everywhere. Settings live in system.json under thistle_os.time.
 * Implemented in components/kernel_rs/src/time_manager.rs; decisions in
 * time_policy.rs, zones in time_zones.rs, ESP-IDF calls in time_platform.c.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What last set or confirmed the clock. */
typedef enum {
    TIME_SOURCE_NONE,     /* never set: the clock reads before 2026 */
    TIME_SOURCE_KEPT,     /* set before the last restart, by what is not known */
    TIME_SOURCE_RTC,      /* a clock chip */
    TIME_SOURCE_NETWORK,  /* the mobile network (NITZ) */
    TIME_SOURCE_GNSS,     /* the GPS receiver */
    TIME_SOURCE_NTP,      /* a time server */
    TIME_SOURCE_MANUAL,   /* the user, with automatic time off */
} time_source_t;

typedef struct {
    bool          known;      /* the clock has been set */
    time_source_t source;
    int64_t       synced_at;  /* when `source` last set it, UTC seconds; 0 if not known */
    bool          automatic;  /* sources set the clock (thistle_os.time.automatic) */
    bool          format_24h;
    bool          has_rtc;    /* the board has a clock chip */
    char          zone[48];   /* the setting: an IANA name, or "" to follow the network */
} time_status_t;

/* ESP_ERR_INVALID_STATE before the kernel has started the clock. */
esp_err_t time_manager_get_status(time_status_t *out);

/* "Set by NTP, 5 min ago", "Not set yet". */
void time_manager_describe(char *buf, size_t len);

/* The zone in force: "Europe/Oslo", or "Automatic (UTC+02:00)". */
void time_manager_zone_label(char *buf, size_t len);

/* The local time of day, "14:05" or "2:05 PM", or "--:--" while the clock
 * is not set. 9 bytes hold either. */
void time_manager_format_time(char *buf, size_t len);

/* The local date, "2026-10-13", or "----/--/--" while the clock is not set. */
void time_manager_format_date(char *buf, size_t len);

/* Each saves to system.json and takes effect at once. */
esp_err_t time_manager_set_automatic(bool on);
/* An IANA name from time_manager_zone_name(), or "" to follow the mobile
 * network's offset (UTC until it gives one). ESP_ERR_INVALID_ARG otherwise. */
esp_err_t time_manager_set_zone(const char *zone);
esp_err_t time_manager_set_24h(bool on);

/* Set the clock by hand, local time as "2026-10-13 14:05" (seconds optional).
 * Only with automatic time off: ESP_ERR_INVALID_STATE while it is on.
 * ESP_ERR_INVALID_ARG for text that is not such a time, or one before 2026. */
esp_err_t time_manager_set_local(const char *text);

/* The zones Settings offers, by IANA name ("Region/City"), sorted within
 * each region, "UTC" first. NULL past the end. */
size_t time_manager_zone_count(void);
const char *time_manager_zone_name(size_t index);

#ifdef __cplusplus
}
#endif
