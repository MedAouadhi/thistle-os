/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Simulator GPS HAL driver — returns a static position (San Francisco by default).
 */
#include "sim_gps.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static double   s_lat  = 37.7749;
static double   s_lon  = -122.4194;
static float    s_alt  = 15.0f;
static uint8_t  s_sats = 10;
static bool     s_fix  = true;

static hal_gps_cb_t s_cb        = NULL;
static void        *s_cb_user   = NULL;

/* ---- vtable functions --------------------------------------------------- */

static esp_err_t sim_gps_init(const void *config)
{
    (void)config;
    return ESP_OK;
}

static void sim_gps_deinit(void)
{
}

static esp_err_t sim_gps_enable(void)
{
    return ESP_OK;
}

static esp_err_t sim_gps_disable(void)
{
    return ESP_OK;
}

static esp_err_t sim_gps_get_position(hal_gps_position_t *pos)
{
    if (!pos) return ESP_ERR_INVALID_ARG;

    pos->latitude    = s_lat;
    pos->longitude   = s_lon;
    pos->altitude_m  = s_alt;
    pos->speed_kmh   = 0.0f;
    pos->heading_deg = 0.0f;
    pos->satellites  = s_sats;
    pos->fix_valid   = s_fix;
    pos->timestamp   = (uint32_t)time(NULL);
    return ESP_OK;
}

/* A sky to go with the position: a few more satellites in view than in use,
 * as a receiver outdoors lists, and none heard without a fix. */
static esp_err_t sim_gps_get_sky(hal_gps_sky_t *sky)
{
    if (!sky) return ESP_ERR_INVALID_ARG;

    sky->in_view  = s_sats + 4;
    sky->heard    = s_fix ? s_sats + 1 : 0;
    sky->best_cno = s_fix ? 42 : 0;
    return ESP_OK;
}

/* What a real receiver's driver would send it, said: the time and the
 * position as given, and how many of the service's messages came, so a test
 * sees what the kernel hands over. A receiver with a fix takes none. */
static esp_err_t sim_gps_assist(const hal_gps_assist_t *aid)
{
    if (!aid) return ESP_ERR_INVALID_ARG;
    if (s_fix) return ESP_OK;

    unsigned messages = 0;
    for (size_t at = 0; aid->data && at + 8 <= aid->data_len;) {
        if (aid->data[at] != 0xB5 || aid->data[at + 1] != 0x62) break;
        size_t end = at + 8 + (aid->data[at + 4] | (aid->data[at + 5] << 8));
        if (end > aid->data_len) break;
        messages += aid->data[at + 2] == 0x13;
        at = end;
    }
    printf("[sim_gps] assistance: time %s, position %s, %u messages\n",
           aid->utc_us ? "given" : "none", aid->has_position ? "given" : "none", messages);
    fflush(stdout);
    return ESP_OK;
}

/* A UBX frame's checksum, over the class, ID, length and payload. */
static void ubx_close(uint8_t *frame, size_t payload_len)
{
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < 6 + payload_len; i++) {
        a += frame[i];
        b += a;
    }
    frame[6 + payload_len] = a;
    frame[7 + payload_len] = b;
}

/* The replies a MIA-M10Q gives to UBX-SEC-UNIQID and UBX-MON-VER. */
static esp_err_t sim_gps_get_identity(hal_gps_identity_t *id)
{
    if (!id) return ESP_ERR_INVALID_ARG;
    memset(id, 0, sizeof(*id));

    static const uint8_t uniq[10] = { 0x02, 0, 0, 0, 0x51, 0x4D, 0x00, 0x01, 0x02, 0x03 };
    memcpy(id->unique_id, (const uint8_t[]){ 0xB5, 0x62, 0x27, 0x03, sizeof(uniq), 0 }, 6);
    memcpy(id->unique_id + 6, uniq, sizeof(uniq));
    ubx_close(id->unique_id, sizeof(uniq));
    id->unique_id_len = 8 + sizeof(uniq);

    static const char *const strings[] = {
        "ROM SPG 5.10 (7b202e)", "00190000", "ROM BASE 0x118B2060",
        "FWVER=SPG 5.10", "PROTVER=34.10", "MOD=MIA-M10Q", "GPS;GLO;GAL;BDS", "SBAS;QZSS",
    };
    enum { STRINGS = sizeof(strings) / sizeof(strings[0]) };
    const size_t payload_len = 40 + 30 * (STRINGS - 2);
    uint8_t *f = id->version;
    memcpy(f, (const uint8_t[]){ 0xB5, 0x62, 0x0A, 0x04, payload_len & 0xFF, payload_len >> 8 }, 6);
    strncpy((char *)f + 6, strings[0], 30);
    strncpy((char *)f + 36, strings[1], 10);
    for (int i = 2; i < STRINGS; i++) {
        strncpy((char *)f + 46 + 30 * (i - 2), strings[i], 30);
    }
    ubx_close(f, payload_len);
    id->version_len = 8 + payload_len;
    return ESP_OK;
}

/* A sky to report satellite by satellite, as the MIA-M10Q's UBX-NAV-SAT
 * does: GPS and Galileo, with predicted orbits for GPS. With a fix, the
 * position's satellites are used; without, a few are heard and none used. */
static esp_err_t sim_gps_get_satellites(hal_gps_sats_t *sats)
{
    if (!sats) return ESP_ERR_INVALID_ARG;
    memset(sats, 0, sizeof(*sats));

    const unsigned listed = s_sats + 4;
    for (unsigned i = 0; i < listed && i < HAL_GPS_SATS_MAX; i++) {
        hal_gps_sat_t *sat = &sats->sats[i];
        const bool gps = i % 3 != 2;
        sat->gnss = gps ? 'G' : 'E';
        sat->sv = (uint8_t)(i + 1);
        sat->elev = (int8_t)(10 + (i * 7) % 80);
        sat->azim = (int16_t)((i * 37) % 360);
        sat->used = s_fix && i < s_sats;
        sat->cno = (s_fix || i < 4) ? (uint8_t)(25 + i % 15) : 0;
        sat->quality = sat->used ? 7 : sat->cno ? 4 : 1;
        sat->health = 1;
        sat->orbits = HAL_GPS_HAS_ALMANAC | (gps ? HAL_GPS_HAS_PREDICTED : 0) |
                      (sat->used ? HAL_GPS_HAS_EPHEMERIS : 0);
        sat->orbit = sat->used ? HAL_GPS_ORBIT_EPHEMERIS
                   : gps       ? HAL_GPS_ORBIT_PREDICTED
                               : HAL_GPS_ORBIT_ALMANAC;
        sats->count++;
    }
    return ESP_OK;
}

static esp_err_t sim_gps_register_callback(hal_gps_cb_t cb, void *user_data)
{
    s_cb      = cb;
    s_cb_user = user_data;
    return ESP_OK;
}

static esp_err_t sim_gps_sleep(bool enter)
{
    (void)enter;
    return ESP_OK;
}

/* ---- driver instance ---------------------------------------------------- */

static const hal_gps_driver_t sim_gps_driver = {
    .init              = sim_gps_init,
    .deinit            = sim_gps_deinit,
    .enable            = sim_gps_enable,
    .disable           = sim_gps_disable,
    .get_position      = sim_gps_get_position,
    .register_callback = sim_gps_register_callback,
    .sleep             = sim_gps_sleep,
    .name              = "Simulator GPS",
    .get_sky           = sim_gps_get_sky,
    .assist            = sim_gps_assist,
    .get_identity      = sim_gps_get_identity,
    .get_satellites    = sim_gps_get_satellites,
};

const hal_gps_driver_t *sim_gps_get(void)
{
    return &sim_gps_driver;
}

void sim_gps_set_position(double lat, double lon, float alt, uint8_t sats, bool fix)
{
    s_lat  = lat;
    s_lon  = lon;
    s_alt  = alt;
    s_sats = sats;
    s_fix  = fix;
}
