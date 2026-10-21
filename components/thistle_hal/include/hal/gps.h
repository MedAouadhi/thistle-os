#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    double latitude;
    double longitude;
    float altitude_m;
    float speed_kmh;
    float heading_deg;
    uint8_t satellites;
    bool fix_valid;
    uint32_t timestamp;   // UTC time as unix timestamp
} hal_gps_position_t;

/* What the receiver hears, fix or not: what tells a receiver that needs a
 * better view of the sky from one that hears nothing at all. */
typedef struct {
    uint8_t in_view;    // satellites it lists above the horizon
    uint8_t heard;      // of those, how many it receives
    uint8_t best_cno;   // the strongest signal, dB-Hz; 0 with none heard
} hal_gps_sky_t;

/* What helps the receiver to its first fix: the time, where it last was,
 * and what its maker's assistance service sends. */
typedef struct {
    int64_t utc_us;           // the time now, µs since 1970; 0 when the clock is not set
    uint32_t utc_acc_ms;      // how far off that may be
    bool has_position;
    double latitude;
    double longitude;
    float altitude_m;
    uint32_t position_acc_m;  // how far from there the receiver may be
    /* The service's data as it came: for a u-blox receiver, AssistNow's
     * UBX-MGA messages. NULL with none. */
    const uint8_t *data;
    size_t data_len;
} hal_gps_assist_t;

/* What the assistance service registers a receiver by: for a u-blox one,
 * its UBX-SEC-UNIQID and UBX-MON-VER replies, whole frames. */
typedef struct {
    uint8_t unique_id[32];
    uint16_t unique_id_len;
    uint8_t version[400];
    uint16_t version_len;
} hal_gps_identity_t;

/* Where the receiver has a satellite's orbit from. */
enum {
    HAL_GPS_ORBIT_NONE = 0,
    HAL_GPS_ORBIT_EPHEMERIS,    // decoded off the satellite
    HAL_GPS_ORBIT_ALMANAC,
    HAL_GPS_ORBIT_PREDICTED,    // a downloaded prediction (AssistNow Offline)
    HAL_GPS_ORBIT_AUTONOMOUS,   // the receiver's own prediction
    HAL_GPS_ORBIT_OTHER,
};

/* Which orbits the receiver holds for a satellite: bits. */
#define HAL_GPS_HAS_EPHEMERIS  0x01
#define HAL_GPS_HAS_ALMANAC    0x02
#define HAL_GPS_HAS_PREDICTED  0x04
#define HAL_GPS_HAS_AUTONOMOUS 0x08

/* One satellite the receiver knows of. */
typedef struct {
    char gnss;          // RINEX's letter: G GPS, E Galileo, R GLONASS, C BeiDou, J QZSS, S SBAS, I NavIC, ? other
    uint8_t sv;         // its number in the constellation
    uint8_t cno;        // its signal, dB-Hz; 0 when not heard
    int8_t elev;        // degrees above the horizon; -128 when not known
    int16_t azim;       // degrees; -1 when not known
    uint8_t quality;    // 0 no signal, 1 searching, 2 acquired, 3 unusable, 4 code locked, 5-7 code and carrier locked
    uint8_t orbit;      // what it is navigated by: HAL_GPS_ORBIT_*
    uint8_t orbits;     // HAL_GPS_HAS_* bits
    uint8_t health;     // 0 not known, 1 healthy, 2 unhealthy
    bool used;          // in the fix
    uint8_t reserved;
} hal_gps_sat_t;

#define HAL_GPS_SATS_MAX 80

typedef struct {
    uint32_t age_ms;    // how long ago the receiver said it
    uint16_t count;
    hal_gps_sat_t sats[HAL_GPS_SATS_MAX];
} hal_gps_sats_t;

typedef void (*hal_gps_cb_t)(const hal_gps_position_t *pos, void *user_data);

typedef struct {
    esp_err_t (*init)(const void *config);
    void (*deinit)(void);
    esp_err_t (*enable)(void);
    esp_err_t (*disable)(void);
    /* ESP_OK whenever the receiver is being read, with or without a fix:
     * fix_valid says which, and satellites is meaningful either way.
     * An error means nothing is reading the receiver. */
    esp_err_t (*get_position)(hal_gps_position_t *pos);
    esp_err_t (*register_callback)(hal_gps_cb_t cb, void *user_data);
    esp_err_t (*sleep)(bool enter);
    const char *name;
    /* The last few seconds' sky, with the same ESP_OK as get_position.
     * NULL when the driver cannot tell. Last, so that no field before it
     * moved. */
    esp_err_t (*get_sky)(hal_gps_sky_t *sky);
    /* Assistance for the receiver's start, sent once it talks; the driver
     * copies what it keeps. Ignored by a receiver with a fix, and dropped
     * when it goes to standby. NULL when the driver takes none. */
    esp_err_t (*assist)(const hal_gps_assist_t *aid);
    /* ESP_ERR_NOT_FOUND until the receiver has answered: ask again later.
     * NULL when the driver cannot tell. */
    esp_err_t (*get_identity)(hal_gps_identity_t *id);
    /* The receiver's last report of each satellite it knows of, with the
     * same ESP_ERR_INVALID_STATE as get_position. Each call asks it for the
     * next, so ESP_ERR_NOT_FOUND until one has come after a start. NULL
     * when the driver cannot tell. */
    esp_err_t (*get_satellites)(hal_gps_sats_t *sats);
} hal_gps_driver_t;
