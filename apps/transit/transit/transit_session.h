/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Transit app's fetching
 *
 * While the app is open, its Provider (transit_provider.h) is asked for
 * the box the map shows on a FreeRTOS task of its own, `transit`, with an
 * 8 KB internal stack: at once, then every interval, the provider's
 * minimum or more. Refresh and a tapped stop ask at once, but never twice
 * within FORCED_GAP_MS. Each fetch holds the device awake. A fetch that
 * fails keeps the last result and backs off: the interval, then twice it,
 * and so on up to 5 minutes; Refresh tries at once. Closing the app stops
 * the task: nothing is fetched while it is closed, and it is never a
 * background check.
 *
 * The UI task hands the box and the tapped stop in, and polls for what
 * came back with transit_session_take(); nothing else is shared.
 *
 * With save_geojson, each result is also written to
 * /sdcard/maps/overlays/transit-<provider>.geojson, which the Maps app
 * shows as an overlay of its own.
 */
#pragma once

#include "transit/transit_provider.h"

#include <stdbool.h>
#include <stdint.h>

#define TRANSIT_BACKOFF_MAX_S 300
#define TRANSIT_FORCED_GAP_MS 2000

typedef struct {
    const transit_provider_t *provider;
    char base_url[TRANSIT_URL_MAX];
    int  interval_s;
    bool save_geojson;
} transit_config_t;

/* Why a fetch was made. */
typedef enum {
    TRANSIT_WHY_OPENING = 0,
    TRANSIT_WHY_INTERVAL,
    TRANSIT_WHY_REFRESH,
    TRANSIT_WHY_STOP,
} transit_why_t;

typedef struct {
    uint32_t        seq;        /* one more for each fetch done */
    bool            ok;         /* the last fetch's outcome */
    transit_why_t   why;
    transit_error_t error;      /* when not ok */
    int             retry_s;    /* when not ok: the next try's delay */
    int64_t         ok_at_us;   /* esp_timer time of the last good one; 0: none yet */
} transit_status_t;

/* Starts fetching; false without the memory or the task. */
bool transit_session_start(const transit_config_t *config);

/* Stops: no fetch starts after this. One under way finishes on its own and
 * the task goes. */
void transit_session_stop(void);

/* The box the map shows now; the first one starts the fetching. */
void transit_session_set_box(const transit_box_t *box);

/* The stop whose departures to fetch too, at once; "" or NULL for none. */
void transit_session_select_stop(const char *stop_id);

/* Fetch at once (Refresh). */
void transit_session_refresh(void);

/* If a fetch finished since *seen: its status, and when it was good, the
 * result in *out; true. *seen moves on. */
bool transit_session_take(uint32_t *seen, transit_status_t *status, transit_result_t *out);
