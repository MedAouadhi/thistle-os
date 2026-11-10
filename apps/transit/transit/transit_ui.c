/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Transit screen
 *
 *   ┌──────────────────────────────┐
 *   │ [status]                [⟳]  │  Refresh: fetch and draw now
 *   │   ▲ M10                      │  vehicles (triangles), over
 *   │        ■ Alexanderplatz      │  stops (squares), over the Pack
 *   │                        [ + ] │
 *   │ ┌──────────────────────────┐ │  a tapped stop: its departures;
 *   │ │ Alexanderplatz           │ │  a tap on the card, or on
 *   │ │ 16:47 M10 Hauptbahnhof +2│ │  nothing, closes it
 *   │ └──────────────────────────┘ │
 *   ├──────────────────────────────┤
 *   │ © OpenStreetMap contributors │  the About line: the Pack's credit
 *   │ Live data: VBB, ...          │  and the Provider's
 *   └──────────────────────────────┘
 *
 * The map is the first *.tmap, by name, in /sdcard/maps/, as the Maps app
 * shows it, or hatching without one. It opens on the GPS fix if a Region
 * holds it, else on the Provider's city if one does, else on the first
 * Region's centre; at z16 (or the Region's deepest level if shallower).
 *
 * config/transit.json on the card says which Provider, and may move it:
 *
 *   {"provider": "berlin", "base_url": "http://homelab:3000",
 *    "min_interval_s": 30, "save_geojson": false}
 *
 * base_url points the Provider at another server speaking its protocol (a
 * self-hosted vbb-rest, say); min_interval_s replaces the Provider's
 * minimum interval, 5 s at least; save_geojson writes each result to
 * /sdcard/maps/overlays/ for the Maps app.
 *
 * The fetching is transit_session.c's, on its own task, while the app is
 * open. The box asked for is the view grown by half of itself each way,
 * so a pan step lands on things already fetched. What comes back is two
 * overlays (map_overlay.h): drawn at once the first time and after
 * Refresh, else under the 20 s rule. A failure keeps what is shown and
 * says why on the status line; vehicles older than 10 minutes go.
 */
#include "transit/transit_app.h"
#include "transit/transit_provider.h"
#include "transit/transit_session.h"

#include "map_engine/map_overlay.h"
#include "map_engine/map_pack.h"
#include "map_engine/map_view.h"
#include "podcasts/pod_json.h"

#include "hal/board.h"
#include "hal/sdcard_path.h"
#include "ui/theme.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "transit";

#define MAPS_DIR        THISTLE_SDCARD "/maps"
#define CONFIG_PATH     THISTLE_SDCARD "/config/transit.json"
#define PACK_SUFFIX     ".tmap"
#define NAME_MAX_LEN    96
#define ABOUT_H         38
#define DEFAULT_ZOOM    16
#define BUTTON          36
#define MARGIN          6
#define POLL_MS         500
#define FIX_POLL_MS     1000
#define MIN_INTERVAL_S  5
#define STALE_US        (10LL * 60 * 1000 * 1000)
#define CONFIG_MAX      2048
#define CARD_TEXT       (TRANSIT_MAX_DEPARTURES * (TRANSIT_LINE_MAX + TRANSIT_NAME_MAX + 16))

LV_FONT_DECLARE(reader_font_16);
LV_FONT_DECLARE(reader_font_16_mono);

typedef enum { CARD_NONE = 0, CARD_STOP, CARD_VEHICLE } card_kind_t;

static struct {
    lv_obj_t      *root;
    lv_obj_t      *about;
    lv_obj_t      *status;
    lv_obj_t      *card;
    lv_obj_t      *card_body;
    map_view_t    *view;
    map_pack_t    *pack;
    map_overlay_t *stops;
    map_overlay_t *vehicles;
    lv_timer_t    *poll;
    lv_timer_t    *fix_poll;
    const transit_provider_t *provider;
    bool           running;
    bool           drawn_once;    /* a result has been drawn */
    bool           vehicles_shown;
    uint32_t       seen;
    transit_box_t  box;
    bool           have_box;
    int64_t        ok_at_us;
    card_kind_t    card_kind;
    char           card_stop[TRANSIT_ID_MAX];
    char           card_title[TRANSIT_NAME_MAX];
    char           status_text[TRANSIT_ERROR_MAX + 16];
    /* PSRAM: what is shown, and what came in. */
    transit_result_t *shown;
    transit_result_t *incoming;
} s_ui;

static lv_style_t s_button_style;
static lv_style_t s_box_style;
static bool s_styles_ready;
static lv_font_t s_about_font;
static char s_card_body[CARD_TEXT];

static void *psram_alloc(size_t len)
{
    void *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(len);
}

/* ------------------------------------------------------------------ */
/* The card's config                                                    */
/* ------------------------------------------------------------------ */

/* config/transit.json into *config; the first Provider, its own URL and
 * interval, without one. False, with why, for a provider it does not
 * know. */
static bool read_config(transit_config_t *config, char *why, size_t why_cap)
{
    memset(config, 0, sizeof *config);
    char provider[24] = "";
    FILE *f = fopen(CONFIG_PATH, "rb");
    if (f) {
        char *text = psram_alloc(CONFIG_MAX);
        size_t len = text ? fread(text, 1, CONFIG_MAX - 1, f) : 0;
        fclose(f);
        if (text) {
            pod_json_t j;
            pod_json_init(&j, text, len);
            char key[24];
            if (pod_json_object(&j)) {
                while (pod_json_key(&j, key, sizeof key)) {
                    int64_t n;
                    if (strcmp(key, "provider") == 0) {
                        pod_json_string(&j, provider, sizeof provider);
                    } else if (strcmp(key, "base_url") == 0) {
                        pod_json_string(&j, config->base_url, sizeof config->base_url);
                    } else if (strcmp(key, "min_interval_s") == 0) {
                        if (pod_json_int(&j, &n)) config->interval_s = (int)n;
                    } else if (strcmp(key, "save_geojson") == 0) {
                        if (pod_json_int(&j, &n)) config->save_geojson = n != 0;
                    } else {
                        pod_json_skip(&j);
                    }
                }
            }
            if (j.error) ESP_LOGW(TAG, "config/transit.json is not JSON; taken as empty");
            free(text);
        }
    }
    config->provider = transit_provider_find(provider);
    if (!config->provider) {
        snprintf(why, why_cap, "config/transit.json names a provider this ThistleOS lacks: %s",
                 provider);
        return false;
    }
    if (!config->base_url[0]) {
        snprintf(config->base_url, sizeof config->base_url, "%s", config->provider->base_url);
    }
    size_t n = strlen(config->base_url);
    while (n && config->base_url[n - 1] == '/') config->base_url[--n] = '\0';
    if (config->interval_s <= 0) config->interval_s = config->provider->min_interval_s;
    if (config->interval_s < MIN_INTERVAL_S) config->interval_s = MIN_INTERVAL_S;
    return true;
}

/* ------------------------------------------------------------------ */
/* The page                                                             */
/* ------------------------------------------------------------------ */

static void init_styles(void)
{
    if (s_styles_ready) return;
    lv_style_init(&s_button_style);
    lv_style_set_bg_color(&s_button_style, lv_color_white());
    lv_style_set_bg_opa(&s_button_style, LV_OPA_COVER);
    lv_style_set_border_color(&s_button_style, lv_color_black());
    lv_style_set_border_width(&s_button_style, 2);
    lv_style_set_radius(&s_button_style, 4);
    lv_style_set_text_color(&s_button_style, lv_color_black());
    lv_style_set_text_font(&s_button_style, &lv_font_montserrat_18);
    lv_style_init(&s_box_style);
    lv_style_set_bg_color(&s_box_style, lv_color_white());
    lv_style_set_bg_opa(&s_box_style, LV_OPA_COVER);
    lv_style_set_border_color(&s_box_style, lv_color_black());
    lv_style_set_border_width(&s_box_style, 2);
    lv_style_set_text_color(&s_box_style, lv_color_black());
    s_styles_ready = true;
}

static void show_message(const char *title, const char *detail)
{
    lv_obj_t *box = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(box, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 8, LV_PART_MAIN);
    lv_obj_center(box);
    lv_obj_t *t = lv_label_create(box);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_label_set_text(t, title);
    lv_obj_t *d = lv_label_create(box);
    lv_obj_set_width(d, LV_PCT(100));
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(d, detail);
}

static void log_centre(const char *what, lv_obj_t *obj)
{
    lv_obj_update_layout(obj);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    ESP_LOGI(TAG, "%s at %d,%d", what, (int)(a.x1 + a.x2) / 2, (int)(a.y1 + a.y2) / 2);
}

static lv_obj_t *make_button(const char *text, lv_align_t align, int x, int y, lv_event_cb_t cb,
                             void *user)
{
    lv_obj_t *button = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(button);
    lv_obj_add_style(button, &s_button_style, LV_PART_MAIN);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(button, BUTTON, BUTTON);
    lv_obj_align(button, align, x, y);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

/* The About line: the Pack's credit, its first sentence, and the
 * Provider's, as their licences ask. */
static void make_about(bool mono)
{
    char line[200];
    const char *credit = s_ui.pack ? map_pack_credit(s_ui.pack) : "";
    const char *stop = strstr(credit, ". ");
    int len = stop ? (int)(stop - credit) : (int)strlen(credit);
    snprintf(line, sizeof line, "%.*s%s%s", len, credit, len ? "\n" : "", s_ui.provider->credit);

    s_about_font = lv_font_montserrat_14;
    s_about_font.fallback = mono ? &reader_font_16_mono : &reader_font_16;

    s_ui.about = lv_label_create(s_ui.root);
    lv_obj_set_size(s_ui.about, LV_PCT(100), ABOUT_H);
    lv_obj_align(s_ui.about, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_ui.about, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.about, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.about, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.about, &s_about_font, LV_PART_MAIN);
    lv_obj_set_style_border_side(s_ui.about, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.about, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.about, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_ui.about, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_ui.about, 2, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_ui.about, -2, LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.about, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_ui.about, line);
}

/* The status line at the top left: why the data is not live, or nothing.
 * Rewritten only when its text changes, each rewrite being a refresh. */
static void set_status(const char *text)
{
    if (strcmp(text, s_ui.status_text) == 0) return;
    snprintf(s_ui.status_text, sizeof s_ui.status_text, "%s", text);
    if (!text[0]) {
        lv_obj_add_flag(s_ui.status, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "status: live");
        return;
    }
    lv_label_set_text(s_ui.status, text);
    lv_obj_remove_flag(s_ui.status, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "status: %s", text);
}

static void make_status(void)
{
    s_ui.status = lv_label_create(s_ui.root);
    lv_obj_add_style(s_ui.status, &s_box_style, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.status, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_ui.status, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_ui.status, 2, LV_PART_MAIN);
    lv_obj_set_width(s_ui.status, LV_PCT(70));
    lv_obj_set_height(s_ui.status, LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_ui.status, LV_LABEL_LONG_DOT);
    lv_obj_align(s_ui.status, LV_ALIGN_TOP_LEFT, MARGIN, MARGIN);
    lv_obj_add_flag(s_ui.status, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ */
/* The card                                                             */
/* ------------------------------------------------------------------ */

static void close_card(void)
{
    if (!s_ui.card) return;
    lv_obj_delete(s_ui.card);
    s_ui.card = NULL;
    s_ui.card_body = NULL;
    if (s_ui.card_kind == CARD_STOP) transit_session_select_stop("");
    s_ui.card_kind = CARD_NONE;
    s_ui.card_stop[0] = '\0';
    ESP_LOGI(TAG, "card closed");
}

static void card_cb(lv_event_t *e)
{
    (void)e;
    close_card();
}

/* A card over the map's bottom: a title and body text. */
static void open_card(card_kind_t kind, const char *title, const char *body)
{
    close_card();
    s_ui.card_kind = kind;
    snprintf(s_ui.card_title, sizeof s_ui.card_title, "%s", title);
    s_ui.card = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(s_ui.card);
    lv_obj_add_style(s_ui.card, &s_box_style, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_ui.card, 2, LV_PART_MAIN);
    lv_obj_set_size(s_ui.card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(s_ui.card, 170, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ui.card, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(s_ui.card, LV_ALIGN_BOTTOM_MID, 0, -ABOUT_H);
    lv_obj_add_flag(s_ui.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ui.card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_ui.card, card_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = lv_label_create(s_ui.card);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_font(t, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_label_set_text(t, title);
    s_ui.card_body = lv_label_create(s_ui.card);
    lv_obj_set_width(s_ui.card_body, lv_pct(100));
    lv_label_set_long_mode(s_ui.card_body, LV_LABEL_LONG_WRAP);
    snprintf(s_card_body, sizeof s_card_body, "%s", body);
    lv_label_set_text_static(s_ui.card_body, s_card_body);
    log_centre("card shown", s_ui.card);
}

/* The departures in the card, if they are the open stop's and say
 * something new. */
static void show_departures(const transit_result_t *r)
{
    if (s_ui.card_kind != CARD_STOP || strcmp(r->departures_for, s_ui.card_stop) != 0) return;
    char body[CARD_TEXT] = "", log[CARD_TEXT + TRANSIT_NAME_MAX + 8];
    size_t used = 0;
    int logged = snprintf(log, sizeof log, "%s", s_ui.card_title);
    for (int i = 0; i < r->departure_count; i++) {
        const transit_departure_t *d = &r->departures[i];
        char late[12] = "";
        if (d->live && d->delay_min > 0) snprintf(late, sizeof late, " +%d", d->delay_min);
        int n = snprintf(body + used, sizeof body - used, "%s%s %s %s%s", i ? "\n" : "", d->when,
                         d->line, d->direction, late);
        if (n < 0 || (size_t)n >= sizeof body - used) break;
        used += (size_t)n;
        int m = snprintf(log + logged, sizeof log - (size_t)logged, "; %s %s %s%s", d->when,
                         d->line, d->direction, late);
        if (m > 0 && (size_t)m < sizeof log - (size_t)logged) logged += m;
    }
    if (r->departure_count == 0) {
        snprintf(body, sizeof body, "No departures in the next 30 minutes");
        snprintf(log + logged, sizeof log - (size_t)logged, "; none");
    }
    if (strcmp(body, s_card_body) == 0) return;
    snprintf(s_card_body, sizeof s_card_body, "%s", body);
    lv_label_set_text_static(s_ui.card_body, s_card_body);
    ESP_LOGI(TAG, "card: %s", log);
}

static void stop_tapped(map_overlay_t *overlay, int item, uint32_t id, void *user)
{
    (void)overlay;
    (void)item;
    (void)user;
    if (id >= (uint32_t)s_ui.shown->stop_count) return;
    const transit_stop_t *st = &s_ui.shown->stops[id];
    open_card(CARD_STOP, st->name, "Departures...");
    snprintf(s_ui.card_stop, sizeof s_ui.card_stop, "%s", st->id);
    ESP_LOGI(TAG, "card: %s; asking for its departures", st->name);
    transit_session_select_stop(st->id);
}

static void vehicle_tapped(map_overlay_t *overlay, int item, uint32_t id, void *user)
{
    (void)overlay;
    (void)item;
    (void)user;
    if (id >= (uint32_t)s_ui.shown->vehicle_count) return;
    const transit_vehicle_t *v = &s_ui.shown->vehicles[id];
    char body[TRANSIT_NAME_MAX + 8];
    snprintf(body, sizeof body, "to %s", v->direction);
    open_card(CARD_VEHICLE, v->line, body);
    ESP_LOGI(TAG, "card: %s %s", v->line, body);
}

static void miss_cb(map_view_t *view, int x, int y, void *user)
{
    (void)view;
    (void)x;
    (void)y;
    (void)user;
    close_card();
}

/* ------------------------------------------------------------------ */
/* Results onto the map                                                 */
/* ------------------------------------------------------------------ */

static bool same_stops(const transit_result_t *a, const transit_result_t *b)
{
    return a->stop_count == b->stop_count &&
           memcmp(a->stops, b->stops, sizeof(transit_stop_t) * (size_t)a->stop_count) == 0;
}

static bool same_vehicles(const transit_result_t *a, const transit_result_t *b)
{
    return a->vehicle_count == b->vehicle_count &&
           memcmp(a->vehicles, b->vehicles,
                  sizeof(transit_vehicle_t) * (size_t)a->vehicle_count) == 0;
}

static void fill_stops(const transit_result_t *r)
{
    map_overlay_clear(s_ui.stops);
    for (int i = 0; i < r->stop_count; i++) {
        const transit_stop_t *st = &r->stops[i];
        map_overlay_add_point(s_ui.stops, st->lat, st->lon, MAP_ICON_SQUARE, st->name,
                              (uint32_t)i);
    }
    map_overlay_changed(s_ui.stops);
}

static void fill_vehicles(const transit_result_t *r)
{
    map_overlay_clear(s_ui.vehicles);
    for (int i = 0; i < r->vehicle_count; i++) {
        const transit_vehicle_t *v = &r->vehicles[i];
        map_overlay_add_point(s_ui.vehicles, v->lat, v->lon, MAP_ICON_TRIANGLE, v->line,
                              (uint32_t)i);
    }
    map_overlay_changed(s_ui.vehicles);
}

static void take_result(const transit_status_t *st)
{
    transit_result_t *r = s_ui.incoming;
    bool stops = !s_ui.drawn_once || !same_stops(r, s_ui.shown);
    bool vehicles = !s_ui.drawn_once || !s_ui.vehicles_shown || !same_vehicles(r, s_ui.shown);
    transit_result_t *was = s_ui.shown;
    s_ui.shown = r;
    s_ui.incoming = was;
    if (stops) fill_stops(r);
    if (vehicles) fill_vehicles(r);
    s_ui.vehicles_shown = true;
    if (stops || vehicles) {
        ESP_LOGI(TAG, "stops: %d; vehicles: %d", r->stop_count, r->vehicle_count);
    }
    /* The first result is what the app was opened for, and Refresh's is
     * what was asked for now: both drawn at once. */
    if (!s_ui.drawn_once || st->why == TRANSIT_WHY_REFRESH) map_view_refresh(s_ui.view);
    s_ui.drawn_once = true;
    show_departures(r);
}

/* The view's box, grown by half of itself each way. */
static void view_box(transit_box_t *box)
{
    int w = map_view_width(s_ui.view), h = map_view_height(s_ui.view);
    double north, west, south, east;
    map_view_from_screen(s_ui.view, -w / 2, -h / 2, &north, &west);
    map_view_from_screen(s_ui.view, w + w / 2, h + h / 2, &south, &east);
    box->north = north;
    box->west = west;
    box->south = south;
    box->east = east;
}

static void poll_cb(lv_timer_t *timer)
{
    (void)timer;
    transit_box_t box;
    view_box(&box);
    if (!s_ui.have_box || memcmp(&box, &s_ui.box, sizeof box) != 0) {
        s_ui.box = box;
        s_ui.have_box = true;
        transit_session_set_box(&box);
    }

    transit_status_t st;
    if (transit_session_take(&s_ui.seen, &st, s_ui.incoming)) {
        if (st.ok) {
            s_ui.ok_at_us = st.ok_at_us;
            take_result(&st);
            set_status("");
        } else {
            set_status(st.error.words);
        }
    }
    /* Vehicles where they were 10 minutes ago mislead: they go. The stops
     * stay. */
    if (s_ui.vehicles_shown && s_ui.ok_at_us &&
        esp_timer_get_time() - s_ui.ok_at_us > STALE_US) {
        map_overlay_clear(s_ui.vehicles);
        map_overlay_changed(s_ui.vehicles);
        s_ui.vehicles_shown = false;
        ESP_LOGI(TAG, "vehicles taken off: the last live data is over 10 minutes old");
    }
}

/* ------------------------------------------------------------------ */
/* Input and the position                                               */
/* ------------------------------------------------------------------ */

static void refresh(void)
{
    ESP_LOGI(TAG, "refresh");
    transit_session_refresh();
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    refresh();
}

static void zoom_cb(lv_event_t *e)
{
    map_view_zoom_by(s_ui.view, (int)(intptr_t)lv_event_get_user_data(e));
}

static bool read_fix(double *lat, double *lon)
{
    const hal_registry_t *reg = hal_get_registry();
    if (!reg || !reg->gps || !reg->gps->get_position) return false;
    hal_gps_position_t pos;
    if (reg->gps->get_position(&pos) != ESP_OK || !pos.fix_valid) return false;
    *lat = pos.latitude;
    *lon = pos.longitude;
    return true;
}

static void fix_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    double lat, lon;
    if (read_fix(&lat, &lon)) map_view_set_position(s_ui.view, lat, lon);
}

static void key_cb(lv_event_t *e)
{
    switch (lv_event_get_key(e)) {
    case 'r': case 'R': refresh(); break;
    case 'l': case 'L': map_view_locate(s_ui.view); break;
    case LV_KEY_ESC: close_card(); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Opening                                                              */
/* ------------------------------------------------------------------ */

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n > m && strcmp(s + n - m, suffix) == 0;
}

/* The Maps app's Pack: the first *.tmap in MAPS_DIR by name. */
static map_pack_t *open_pack(void)
{
    DIR *dir = opendir(MAPS_DIR);
    if (!dir) return NULL;
    char name[NAME_MAX_LEN] = "";
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.' || !ends_with(e->d_name, PACK_SUFFIX)) continue;
        if (strlen(e->d_name) >= sizeof name) continue;
        if (!name[0] || strcmp(e->d_name, name) < 0) snprintf(name, sizeof name, "%s", e->d_name);
    }
    closedir(dir);
    if (!name[0]) {
        ESP_LOGI(TAG, "no Map Pack in /sdcard/maps: the map is hatched");
        return NULL;
    }
    char path[sizeof MAPS_DIR + NAME_MAX_LEN + 1];
    snprintf(path, sizeof path, "%s/%s", MAPS_DIR, name);
    map_pack_t *pack = NULL;
    unsigned newer = 0;
    map_pack_err_t err = map_pack_open(path, &pack, &newer);
    if (err != MAP_PACK_OK) {
        ESP_LOGW(TAG, "cannot open %s: %s; the map is hatched", name, map_pack_err_text(err));
        return NULL;
    }
    ESP_LOGI(TAG, "map: %s", name);
    return pack;
}

/* The deepest level up to z16 at which the Pack has the tile holding
 * (lat, lon); 0 if none. */
static int zoom_holding(double lat, double lon)
{
    if (!s_ui.pack) return 0;
    for (int z = DEFAULT_ZOOM; z >= MAP_MIN_ZOOM; z--) {
        double gx, gy;
        map_world_pixel(lat, lon, z, &gx, &gy);
        if (gx < 0 || gy < 0) return 0;
        if (map_pack_has_tile(s_ui.pack, z, (uint32_t)(gx / MAP_TILE_PX),
                              (uint32_t)(gy / MAP_TILE_PX))) {
            return z;
        }
    }
    return 0;
}

static void go_to_start(void)
{
    double lat, lon;
    int zoom = 0;
    const char *where = "your position";
    if (read_fix(&lat, &lon)) {
        map_view_set_position(s_ui.view, lat, lon);
        zoom = zoom_holding(lat, lon);
    }
    if (!zoom) {
        lat = s_ui.provider->lat;
        lon = s_ui.provider->lon;
        where = s_ui.provider->name;
        zoom = zoom_holding(lat, lon);
    }
    if (!zoom && s_ui.pack && map_pack_region_count(s_ui.pack) > 0) {
        const map_region_t *first = map_pack_region(s_ui.pack, 0);
        lat = first->centre_lat;
        lon = first->centre_lon;
        where = first->name;
        zoom = first->deepest_zoom < DEFAULT_ZOOM ? first->deepest_zoom : DEFAULT_ZOOM;
    }
    if (!zoom) zoom = DEFAULT_ZOOM;
    map_view_go_to(s_ui.view, lat, lon, zoom);
    ESP_LOGI(TAG, "showing z%d around %.7f, %.7f (%s)", zoom, lat, lon, where);
}

esp_err_t transit_ui_create(lv_obj_t *parent)
{
    if (!parent) parent = lv_screen_active();
    memset(&s_ui, 0, sizeof s_ui);
    init_styles();

    const hal_registry_t *reg = hal_get_registry();
    bool mono = reg && reg->display && reg->display->type == HAL_DISPLAY_TYPE_EPAPER;
    const theme_colors_t *clr = theme_get_colors();
    s_ui.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.root);
    lv_obj_set_size(s_ui.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ui.root, clr->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.root, clr->text, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.root, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    transit_config_t config;
    char why[128];
    if (!read_config(&config, why, sizeof why)) {
        ESP_LOGW(TAG, "%s", why);
        show_message("No transit here", why);
        return ESP_OK;
    }
    s_ui.provider = config.provider;

    s_ui.shown = psram_alloc(sizeof *s_ui.shown);
    s_ui.incoming = psram_alloc(sizeof *s_ui.incoming);
    if (s_ui.shown) memset(s_ui.shown, 0, sizeof *s_ui.shown);
    s_ui.pack = open_pack();
    if (s_ui.shown && s_ui.incoming) {
        s_ui.view = map_view_create(s_ui.root, s_ui.pack, NULL, NULL);
    }
    if (!s_ui.view) {
        ESP_LOGE(TAG, "no memory for the map");
        show_message("Cannot show the map", "There is not enough memory for the map.");
        return ESP_OK;
    }
    map_view_set_bottom_inset(s_ui.view, ABOUT_H);
    go_to_start();

    s_ui.stops = map_overlay_create(s_ui.view, "stops", 1, stop_tapped, NULL);
    s_ui.vehicles = map_overlay_create(s_ui.view, "vehicles", 2, vehicle_tapped, NULL);
    map_view_set_miss_cb(s_ui.view, miss_cb, NULL);
    lv_obj_add_event_cb(map_view_obj(s_ui.view), key_cb, LV_EVENT_KEY, NULL);

    lv_obj_t *refresh_button = make_button(LV_SYMBOL_REFRESH, LV_ALIGN_TOP_RIGHT, -MARGIN, MARGIN,
                                           refresh_cb, NULL);
    lv_obj_t *out = make_button("-", LV_ALIGN_BOTTOM_RIGHT, -MARGIN, -(ABOUT_H + MARGIN), zoom_cb,
                                (void *)(intptr_t)-1);
    lv_obj_t *in = make_button("+", LV_ALIGN_BOTTOM_RIGHT, -MARGIN,
                               -(ABOUT_H + MARGIN * 2 + BUTTON), zoom_cb, (void *)(intptr_t)1);
    make_status();
    make_about(mono);

    lv_obj_update_layout(s_ui.root);
    lv_area_t at;
    lv_obj_get_coords(map_view_obj(s_ui.view), &at);
    ESP_LOGI(TAG, "view %dx%d at %d,%d", map_view_width(s_ui.view), map_view_height(s_ui.view),
             (int)at.x1, (int)at.y1);
    log_centre("refresh", refresh_button);
    log_centre("zoom in", in);
    log_centre("zoom out", out);

    if (!s_ui.stops || !s_ui.vehicles || !transit_session_start(&config)) {
        ESP_LOGE(TAG, "cannot start fetching: no memory");
        set_status("Out of memory");
        return ESP_OK;
    }
    s_ui.running = true;
    s_ui.poll = lv_timer_create(poll_cb, POLL_MS, NULL);
    s_ui.fix_poll = lv_timer_create(fix_poll_cb, FIX_POLL_MS, NULL);
    poll_cb(NULL);  /* the box now: the first fetch need not wait */
    return ESP_OK;
}

void transit_ui_destroy(void)
{
    if (s_ui.running) transit_session_stop();
    if (s_ui.poll) lv_timer_delete(s_ui.poll);
    if (s_ui.fix_poll) lv_timer_delete(s_ui.fix_poll);
    /* The overlays before their view. */
    map_overlay_destroy(s_ui.vehicles);
    map_overlay_destroy(s_ui.stops);
    map_view_destroy(s_ui.view);
    if (s_ui.root) lv_obj_delete(s_ui.root);
    map_pack_close(s_ui.pack);
    free(s_ui.shown);
    free(s_ui.incoming);
    memset(&s_ui, 0, sizeof s_ui);
}
