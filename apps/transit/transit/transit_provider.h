/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — a Transit Provider: one city's live transit data
 *
 * A Provider is what the Transit app knows of one city: where its data
 * comes from and how to read it. It has one call, fetch(): given the box
 * the map shows and, when the user has tapped one, a stop, it fills a
 * transit_result_t with the stops in the box, the vehicles moving in it
 * and that stop's next departures. With it, the shortest time it allows
 * between fetches.
 *
 * Adding a city is one new file, transit/providers/<city>.c, defining a
 * `const transit_provider_t transit_provider_<city>`, and its name in
 * TRANSIT_PROVIDERS below. docs/maps/transit-providers.md has the rules
 * and a worked example; providers/berlin.c is the first.
 *
 * fetch() runs on the Transit app's own task (transit_session.c), never
 * the UI task, only while the app is open, with the device held awake.
 * It may block, on the network say, for as long as it needs; it is never
 * called twice at once. transit_net.h has an HTTP GET for it, and the
 * Podcasts app's pod_json.h a JSON reader that allocates nothing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a result holds at most: the Overlay's 256 items are far more than a
 * 240x320 screen shows. */
#define TRANSIT_MAX_STOPS       64
#define TRANSIT_MAX_VEHICLES    64
#define TRANSIT_MAX_DEPARTURES  8

#define TRANSIT_ID_MAX     32
#define TRANSIT_NAME_MAX   48
#define TRANSIT_LINE_MAX   12
#define TRANSIT_URL_MAX    160
#define TRANSIT_ERROR_MAX  96

/* The area to cover, in degrees. */
typedef struct {
    double south, west, north, east;
} transit_box_t;

typedef struct {
    char   id[TRANSIT_ID_MAX];      /* the provider's own, for departures */
    char   name[TRANSIT_NAME_MAX];
    double lat, lon;
} transit_stop_t;

typedef struct {
    char   line[TRANSIT_LINE_MAX];      /* "M10" */
    char   direction[TRANSIT_NAME_MAX]; /* "S+U Hauptbahnhof" */
    double lat, lon;
} transit_vehicle_t;

typedef struct {
    char    when[6];                     /* "16:45", local to the city */
    int     delay_min;                   /* minutes late, when live */
    bool    live;                        /* a forecast, not only the plan */
    char    line[TRANSIT_LINE_MAX];
    char    direction[TRANSIT_NAME_MAX];
} transit_departure_t;

typedef struct {
    transit_box_t box;
    /* The stop whose departures to fetch, as a transit_stop_t's id; ""
     * for none. */
    char    stop_id[TRANSIT_ID_MAX];
    /* config/transit.json's base_url, or the provider's base_url. */
    char    base_url[TRANSIT_URL_MAX];
    /* UNIX seconds, 0 while the clock is not set. */
    int64_t now;
} transit_request_t;

typedef struct {
    transit_stop_t      stops[TRANSIT_MAX_STOPS];
    int                 stop_count;
    transit_vehicle_t   vehicles[TRANSIT_MAX_VEHICLES];
    int                 vehicle_count;
    /* For request->stop_id, when one was asked for. */
    char                departures_for[TRANSIT_ID_MAX];
    transit_departure_t departures[TRANSIT_MAX_DEPARTURES];
    int                 departure_count;
} transit_result_t;

/* Why a fetch failed: words for the user ("No network"), and the HTTP
 * status, 0 when there was no answer, for the log. */
typedef struct {
    char words[TRANSIT_ERROR_MAX];
    int  http_status;
} transit_error_t;

typedef struct transit_provider {
    const char *id;           /* "berlin": config/transit.json's provider */
    const char *name;         /* "Berlin" */
    /* Who the data is from, for the About line: its licence may ask. */
    const char *credit;
    const char *base_url;     /* where it fetches from, unless the card says */
    double      lat, lon;     /* the city's centre, to open on */
    /* The shortest time between fetches it allows, in seconds: what its
     * server's terms or caching make useful. */
    int         min_interval_s;
    /* Bytes of the provider's own state kept between fetches, zeroed
     * when the app opens, freed when it closes; 0 for none. In PSRAM. */
    size_t      state_size;
    /* Fills `out`, which comes zeroed, for `request`; true if it did. On
     * failure it fills `error`, and the app keeps the last result. */
    bool (*fetch)(void *state, const transit_request_t *request, transit_result_t *out,
                  transit_error_t *error);
} transit_provider_t;

/* Every city, by its file in providers/. A new one goes here too. */
#define TRANSIT_PROVIDERS(X) \
    X(berlin)

#define TRANSIT_DECLARE_PROVIDER(city) extern const transit_provider_t transit_provider_##city;
TRANSIT_PROVIDERS(TRANSIT_DECLARE_PROVIDER)
#undef TRANSIT_DECLARE_PROVIDER

/* The provider whose id is `id`; the first one for NULL or "". NULL if
 * none has it. */
const transit_provider_t *transit_provider_find(const char *id);

#ifdef __cplusplus
}
#endif
