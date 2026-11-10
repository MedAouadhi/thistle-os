/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Berlin Provider: VBB's live data through vbb-rest
 *
 * vbb-rest 6 (https://v6.vbb.transport.rest, derhuerst/vbb-rest), a REST
 * front for VBB's own app API: no key, 100 requests a minute, answers
 * cached for 30 s. Ticket 07's research (.scratch/maps/research/
 * berlin-transit-api.md) has the reasons and the fields. A fetch is:
 *
 *   GET /locations/nearby?latitude=&longitude=&distance=&results=  the stops,
 *       around the box's middle; asked again only once the box has moved
 *       a quarter of itself, or after 5 minutes (the server caches 5)
 *   GET /radar?north=&west=&south=&east=&results=&frames=0          vehicles
 *   GET /stops/:id/departures?duration=&results=                    when a
 *       stop is tapped
 *
 * config/transit.json's base_url points it at a self-hosted vbb-rest,
 * on the homelab over the tailnet say: the protocol is the same.
 */
#include "transit/transit_net.h"
#include "transit/transit_provider.h"

#include "podcasts/pod_json.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "transit";

#define STOPS_AGAIN_US    (300LL * 1000 * 1000)
#define STOPS_RADIUS_MAX  2000   /* metres */
#define DEPARTURES_MIN    30     /* minutes ahead */

typedef struct {
    transit_stop_t stops[TRANSIT_MAX_STOPS];
    int            stop_count;
    bool           have_stops;
    double         asked_lat, asked_lon;   /* where they were asked for */
    double         asked_span_lat, asked_span_lon;
    int64_t        asked_us;
} berlin_state_t;

/* ------------------------------------------------------------------ */
/* Reading                                                              */
/* ------------------------------------------------------------------ */

static bool read_double(pod_json_t *j, double *out)
{
    char buf[40];
    if (!pod_json_string(j, buf, sizeof buf) || !buf[0]) return false;
    char *end;
    *out = strtod(buf, &end);
    return end != buf;
}

/* {"latitude": .., "longitude": ..}; false without both. */
static bool read_location(pod_json_t *j, double *lat, double *lon)
{
    bool has_lat = false, has_lon = false;
    char key[16];
    if (!pod_json_object(j)) return false;
    while (pod_json_key(j, key, sizeof key)) {
        if (strcmp(key, "latitude") == 0) has_lat = read_double(j, lat);
        else if (strcmp(key, "longitude") == 0) has_lon = read_double(j, lon);
        else pod_json_skip(j);
    }
    return has_lat && has_lon && *lat >= -85 && *lat <= 85 && *lon >= -180 && *lon <= 180;
}

/* A line's "name". */
static void read_line(pod_json_t *j, char *name, size_t cap)
{
    char key[16];
    if (!pod_json_object(j)) return;
    while (pod_json_key(j, key, sizeof key)) {
        if (strcmp(key, "name") == 0) pod_json_string(j, name, cap);
        else pod_json_skip(j);
    }
}

/* Enters the list an answer holds: the array under `key` of an object, or
 * the answer itself when it is an array (vbb-rest 5 answered so). */
static bool enter_list(pod_json_t *j, const char *key)
{
    pod_json_t probe = *j;
    if (pod_json_array(&probe)) {
        *j = probe;
        return true;
    }
    char k[24];
    if (!pod_json_object(j)) return false;
    while (pod_json_key(j, k, sizeof k)) {
        if (strcmp(k, key) == 0) return pod_json_array(j);
        pod_json_skip(j);
    }
    return false;
}

static bool read_stops(const char *body, size_t len, berlin_state_t *st)
{
    pod_json_t j;
    pod_json_init(&j, body, len);
    if (!pod_json_array(&j)) return false;
    st->stop_count = 0;
    while (pod_json_next(&j)) {
        transit_stop_t stop = {0};
        char key[16], type[16] = "";
        bool placed = false;
        if (!pod_json_object(&j)) continue;
        while (pod_json_key(&j, key, sizeof key)) {
            if (strcmp(key, "id") == 0) pod_json_string(&j, stop.id, sizeof stop.id);
            else if (strcmp(key, "name") == 0) pod_json_string(&j, stop.name, sizeof stop.name);
            else if (strcmp(key, "type") == 0) pod_json_string(&j, type, sizeof type);
            else if (strcmp(key, "location") == 0) placed = read_location(&j, &stop.lat, &stop.lon);
            else pod_json_skip(&j);
        }
        bool is_stop = !type[0] || strcmp(type, "stop") == 0 || strcmp(type, "station") == 0;
        if (placed && is_stop && stop.id[0] && st->stop_count < TRANSIT_MAX_STOPS) {
            st->stops[st->stop_count++] = stop;
        }
    }
    return !j.error;
}

static bool read_movements(const char *body, size_t len, transit_result_t *out)
{
    pod_json_t j;
    pod_json_init(&j, body, len);
    if (!enter_list(&j, "movements")) return false;
    while (pod_json_next(&j)) {
        transit_vehicle_t v = {0};
        char key[24];
        bool placed = false;
        if (!pod_json_object(&j)) continue;
        while (pod_json_key(&j, key, sizeof key)) {
            if (strcmp(key, "line") == 0) read_line(&j, v.line, sizeof v.line);
            else if (strcmp(key, "direction") == 0) pod_json_string(&j, v.direction, sizeof v.direction);
            else if (strcmp(key, "location") == 0) placed = read_location(&j, &v.lat, &v.lon);
            else pod_json_skip(&j);
        }
        if (placed && out->vehicle_count < TRANSIT_MAX_VEHICLES) {
            out->vehicles[out->vehicle_count++] = v;
        }
    }
    return !j.error;
}

/* "2026-11-10T16:45:00+01:00" to "16:45": the city's own time, as the
 * server gives it. */
static bool clock_of(const char *iso, char out[6])
{
    if (strlen(iso) < 16 || iso[10] != 'T' || iso[13] != ':') return false;
    memcpy(out, iso + 11, 5);
    out[5] = '\0';
    return true;
}

static bool read_departures(const char *body, size_t len, transit_result_t *out)
{
    pod_json_t j;
    pod_json_init(&j, body, len);
    if (!enter_list(&j, "departures")) return false;
    while (pod_json_next(&j)) {
        transit_departure_t d = {0};
        char key[24], when[40] = "", planned[40] = "";
        bool has_delay = false;
        int64_t delay = 0;
        if (!pod_json_object(&j)) continue;
        while (pod_json_key(&j, key, sizeof key)) {
            if (strcmp(key, "when") == 0) {
                pod_json_string(&j, when, sizeof when);
            } else if (strcmp(key, "plannedWhen") == 0) {
                pod_json_string(&j, planned, sizeof planned);
            } else if (strcmp(key, "delay") == 0) {
                if (!pod_json_null(&j)) has_delay = pod_json_int(&j, &delay);
            } else if (strcmp(key, "line") == 0) {
                read_line(&j, d.line, sizeof d.line);
            } else if (strcmp(key, "direction") == 0) {
                pod_json_string(&j, d.direction, sizeof d.direction);
            } else {
                pod_json_skip(&j);
            }
        }
        /* Without realtime, `when` is null or the plan, and `delay` null:
         * the planned time, unmarked. A cancelled one has neither. */
        if (!clock_of(when[0] ? when : planned, d.when)) continue;
        d.live = when[0] && has_delay;
        d.delay_min = d.live ? (int)(delay / 60) : 0;
        if (out->departure_count < TRANSIT_MAX_DEPARTURES) {
            out->departures[out->departure_count++] = d;
        }
    }
    return !j.error;
}

/* ------------------------------------------------------------------ */
/* Fetching                                                             */
/* ------------------------------------------------------------------ */

static bool get(const char *url, bool (*read)(const char *, size_t, void *), void *into,
                transit_error_t *error)
{
    size_t len;
    char *body = transit_net_get(url, &len, error);
    if (!body) return false;
    bool ok = read(body, len, into);
    free(body);
    if (!ok) {
        error->http_status = 200;
        snprintf(error->words, sizeof error->words, "The transit service's answer is not JSON");
    }
    return ok;
}

static bool read_stops_cb(const char *b, size_t n, void *st) { return read_stops(b, n, st); }
static bool read_movements_cb(const char *b, size_t n, void *o) { return read_movements(b, n, o); }
static bool read_departures_cb(const char *b, size_t n, void *o) { return read_departures(b, n, o); }

/* Only what a stop id may hold, so it cannot leave its path. */
static void path_escape(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; *in && n + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '-' || c == '_' || c == '.') {
            out[n++] = (char)c;
        } else {
            n += (size_t)snprintf(out + n, cap - n, "%%%02X", c);
        }
    }
    out[n] = '\0';
}

static bool stops_due(const berlin_state_t *st, double lat, double lon)
{
    if (!st->have_stops) return true;
    if (esp_timer_get_time() - st->asked_us >= STOPS_AGAIN_US) return true;
    return fabs(lat - st->asked_lat) > st->asked_span_lat / 4 ||
           fabs(lon - st->asked_lon) > st->asked_span_lon / 4;
}

static bool berlin_fetch(void *state, const transit_request_t *rq, transit_result_t *out,
                         transit_error_t *error)
{
    berlin_state_t *st = state;
    const transit_box_t *b = &rq->box;
    double lat = (b->north + b->south) / 2, lon = (b->east + b->west) / 2;
    char url[TRANSIT_URL_MAX + 256];

    if (stops_due(st, lat, lon)) {
        /* Half the box's diagonal, in metres. */
        double dy = (b->north - b->south) * 111320.0;
        double dx = (b->east - b->west) * 111320.0 * cos(lat * M_PI / 180.0);
        int radius = (int)(sqrt(dx * dx + dy * dy) / 2);
        if (radius > STOPS_RADIUS_MAX) radius = STOPS_RADIUS_MAX;
        snprintf(url, sizeof url,
                 "%s/locations/nearby?latitude=%.6f&longitude=%.6f&distance=%d&results=%d"
                 "&stops=true&poi=false&addresses=false&linesOfStops=false&pretty=false",
                 rq->base_url, lat, lon, radius, TRANSIT_MAX_STOPS);
        if (!get(url, read_stops_cb, st, error)) return false;
        st->have_stops = true;
        st->asked_lat = lat;
        st->asked_lon = lon;
        st->asked_span_lat = b->north - b->south;
        st->asked_span_lon = b->east - b->west;
        st->asked_us = esp_timer_get_time();
    }
    memcpy(out->stops, st->stops, sizeof(transit_stop_t) * (size_t)st->stop_count);
    out->stop_count = st->stop_count;

    snprintf(url, sizeof url,
             "%s/radar?north=%.6f&west=%.6f&south=%.6f&east=%.6f&results=%d&frames=0"
             "&polylines=false&pretty=false",
             rq->base_url, b->north, b->west, b->south, b->east, TRANSIT_MAX_VEHICLES);
    if (!get(url, read_movements_cb, out, error)) return false;

    if (rq->stop_id[0]) {
        char id[TRANSIT_ID_MAX * 3];
        path_escape(rq->stop_id, id, sizeof id);
        snprintf(url, sizeof url,
                 "%s/stops/%s/departures?duration=%d&results=%d&remarks=false"
                 "&linesOfStops=false&pretty=false",
                 rq->base_url, id, DEPARTURES_MIN, TRANSIT_MAX_DEPARTURES);
        if (!get(url, read_departures_cb, out, error)) return false;
        snprintf(out->departures_for, sizeof out->departures_for, "%s", rq->stop_id);
    }
    ESP_LOGD(TAG, "berlin: %d stops, %d vehicles", out->stop_count, out->vehicle_count);
    return true;
}

const transit_provider_t transit_provider_berlin = {
    .id = "berlin",
    .name = "Berlin",
    .credit = "Live data: VBB, via v6.vbb.transport.rest",
    .base_url = "https://v6.vbb.transport.rest",
    .lat = 52.5219,
    .lon = 13.4132,
    /* The server caches /radar and departures for 30 s: asking sooner
     * returns the same. */
    .min_interval_s = 30,
    .state_size = sizeof(berlin_state_t),
    .fetch = berlin_fetch,
};
