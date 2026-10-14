/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the SoC side of the system clock, for the time manager
 *
 * Every call into ESP-IDF's time and SNTP APIs is here, so that
 * time_manager.rs keeps to deciding and the simulators need none of it.
 *
 * - SNTP is esp_netif's client, asking one server every hour
 *   (CONFIG_LWIP_SNTP_UPDATE_DELAY), and at once whenever WiFi or the PPP
 *   link gets an address. ESP-IDF does not apply its answers: it declares
 *   sntp_sync_time() weak so that an application can (esp_sntp.h), and the
 *   one here hands each answer to the kernel, which steps or slews the clock
 *   as it does for every other source. ESP-IDF's own smooth mode would slew
 *   anything under 35 minutes, at 1/64, which takes a day and a half.
 *
 * - The clock is stepped with settimeofday() and slewed with adjtime().
 *
 * - What last set the clock, and when, is kept in RTC memory, which keeps
 *   through deep sleep and a restart. So does the clock itself
 *   (CONFIG_LIBC_TIME_SYSCALL_USE_RTC_HRT); a power-on clears both.
 */

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_rom_crc.h"
#include "esp_sntp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "time";

/* time_manager.rs */
void time_manager_ntp_synced(int64_t seconds, int32_t microseconds);

/* lwIP keeps a pointer to the server's name rather than a copy. */
static char s_server[64];
static bool s_sntp_on;

static void got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    /* A new address may be a new network: ask now rather than within the
     * hour. This restarts the client, which waits up to
     * CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY before asking. */
    esp_netif_sntp_start();
}

esp_err_t time_platform_sntp_start(const char *server)
{
    if (s_sntp_on) {
        return esp_netif_sntp_start();
    }
    /* Both are up once WiFi is; neither minds being asked again. */
    esp_err_t ret = esp_netif_init();
    if (ret == ESP_OK) {
        ret = esp_event_loop_create_default();
    }
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "NTP not started, no network stack: %s", esp_err_to_name(ret));
        return ret;
    }
    strlcpy(s_server, server, sizeof(s_server));
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_server);
    cfg.wait_for_sync = false;
    ret = esp_netif_sntp_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NTP not started: %s", esp_err_to_name(ret));
        return ret;
    }
    s_sntp_on = true;
    if (esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip, NULL) != ESP_OK
        || esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, got_ip, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "NTP will not hear of new addresses; it asks hourly");
    }
    ESP_LOGI(TAG, "NTP from %s", s_server);
    return ESP_OK;
}

void time_platform_sntp_stop(void)
{
    if (!s_sntp_on) {
        return;
    }
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_GOT_IP, got_ip);
    esp_netif_sntp_deinit();
    s_sntp_on = false;
    ESP_LOGI(TAG, "NTP stopped");
}

/* Called by lwIP's SNTP client, on the TCP/IP thread, with each answer. */
void sntp_sync_time(struct timeval *tv)
{
    time_manager_ntp_synced((int64_t)tv->tv_sec, (int32_t)tv->tv_usec);
    sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
}

static int64_t now_us(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (int64_t)now.tv_sec * 1000000 + now.tv_usec;
}

esp_err_t time_platform_set(int64_t utc_us, bool slew)
{
    if (slew) {
        int64_t delta = utc_us - now_us();
        struct timeval d = {
            .tv_sec = (time_t)(delta / 1000000),
            .tv_usec = (suseconds_t)(delta % 1000000),
        };
        if (adjtime(&d, NULL) == 0) {
            return ESP_OK;
        }
        /* More than adjtime takes: step instead. */
    }
    struct timeval tv = {
        .tv_sec = (time_t)(utc_us / 1000000),
        .tv_usec = (suseconds_t)(utc_us % 1000000),
    };
    return settimeofday(&tv, NULL) == 0 ? ESP_OK : ESP_FAIL;
}

/* What a slew under way has yet to add, in microseconds. */
int64_t time_platform_pending_slew_us(void)
{
    struct timeval left;
    if (adjtime(NULL, &left) != 0) {
        return 0;
    }
    return (int64_t)left.tv_sec * 1000000 + left.tv_usec;
}

/* -------------------------------------------------------------------------
 * The record
 * ---------------------------------------------------------------------- */

#define RECORD_MAGIC 0x454d4954u /* "TIME" */

typedef struct {
    uint32_t magic;
    uint32_t source; /* time_source_t; TIME_SOURCE_NONE before any */
    int64_t at_us;
    int32_t offset_min; /* the mobile network's offset; INT32_MIN for none */
    uint32_t crc;
} time_record_t;

/* Not cleared at boot; at a power-on it holds noise, which the CRC rejects. */
static RTC_NOINIT_ATTR time_record_t s_record;

static uint32_t record_crc(const time_record_t *r)
{
    return esp_rom_crc32_le(0, (const uint8_t *)r, offsetof(time_record_t, crc));
}

bool time_platform_record_load(uint8_t *source, int64_t *at_us, int32_t *offset_min)
{
    if (s_record.magic != RECORD_MAGIC || s_record.crc != record_crc(&s_record)) {
        return false;
    }
    *source = (uint8_t)s_record.source;
    *at_us = s_record.at_us;
    *offset_min = s_record.offset_min;
    return true;
}

void time_platform_record_store(uint8_t source, int64_t at_us, int32_t offset_min)
{
    time_record_t r = {
        .magic = RECORD_MAGIC, .source = source, .at_us = at_us, .offset_min = offset_min,
    };
    r.crc = record_crc(&r);
    s_record = r;
}
