/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Maps screen
 *
 *   ┌──────────────────────────────┐
 *   │                     [Tracks] │  the Navigator's tracks on or off
 *   │     the Map Pack's tiles     │  opens on the GPS fix if a Region
 *   │         (map_engine)    (o)  │  holds it, else on the first
 *   │                              │  Region's centre; at z16 (or the
 *   │                        [ + ] │  Region's deepest level, if shallower)
 *   │ [ ⌖ ]                  [ - ] │  locate; zoom in and out
 *   ├──────────────────────────────┤
 *   │ © OpenStreetMap contributors │  the About line: the Pack's credit
 *   └──────────────────────────────┘
 *
 * The Pack is the first *.tmap, by name, in /sdcard/maps/. Without one, or
 * with one that does not open, the page says why instead.
 *
 * A swipe or the keys pan half a screen; the zoom buttons and keys go
 * through the four levels (map_view.h). The GPS fix is read once a second
 * and handed to the view, which draws the marker (o) and keeps it in view
 * as the device moves; locate, its button or the l key, centres on it.
 *
 * The tracks the Navigator recorded, every *.gpx in /sdcard/tracks/, are
 * one overlay, shown at first; the Tracks button or the t key switches
 * it. Other overlays are the next ticket's (.scratch/maps/).
 */
#include "maps/maps_app.h"

#include "map_engine/map_overlay.h"
#include "map_engine/map_pack.h"
#include "map_engine/map_view.h"

#include "hal/board.h"
#include "hal/sdcard_path.h"
#include "thistle/gps_manager.h"
#include "ui/theme.h"
#include "ui/toast.h"

#include "esp_log.h"
#include "lvgl.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "maps";

#define MAPS_DIR       THISTLE_SDCARD "/maps"
#define MAPS_DIR_SHOWN "/sdcard/maps"   /* what the user is told */
#define PACK_SUFFIX    ".tmap"
#define NAME_MAX_LEN   96
#define ABOUT_H        22
#define DEFAULT_ZOOM   16
#define ZOOM_BUTTON    36   /* px a side */
#define ZOOM_MARGIN    6
#define FIX_POLL_MS    1000
#define TRACKS_DIR       THISTLE_SDCARD "/tracks"
#define TRACKS_DIR_SHOWN "/sdcard/tracks"
#define TRACK_SUFFIX     ".gpx"
#define GPX_TAG_MAX      256   /* longer tags are not a trkpt we read */
#define GPX_CHUNK        2048

/* The Reader's font, Latin-1 with it, for the credit's ©, which Montserrat
 * lacks: the Podcasts app borrows it the same way. */
LV_FONT_DECLARE(reader_font_16);
LV_FONT_DECLARE(reader_font_16_mono);

static struct {
    lv_obj_t   *root;
    lv_obj_t   *about;
    map_view_t *view;
    map_pack_t *pack;
    lv_timer_t *fix_poll;
    map_overlay_t *tracks;
    lv_obj_t   *tracks_button;
    bool        tracks_button_on;  /* as drawn */
} s_ui;

/* The buttons' look: shared, and with no pressed state, as a press
 * that drew would be a refresh of its own on e-paper. */
static lv_style_t s_button_style;
static bool s_button_style_ready;

/* Montserrat 14 falling back on the Reader's font: a copy, so LVGL's own
 * keeps no fallback. */
static lv_font_t s_about_font;

/* ------------------------------------------------------------------ */
/* Finding the Pack                                                     */
/* ------------------------------------------------------------------ */

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n > m && strcmp(s + n - m, suffix) == 0;
}

/* The first *.tmap in MAPS_DIR by name, into name; false if there is none. */
static bool find_pack(char *name, size_t cap)
{
    DIR *dir = opendir(MAPS_DIR);
    if (!dir) return false;
    bool found = false;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.' || !ends_with(e->d_name, PACK_SUFFIX)) continue;
        if (strlen(e->d_name) >= cap) continue;
        if (!found || strcmp(e->d_name, name) < 0) {
            snprintf(name, cap, "%s", e->d_name);
            found = true;
        }
    }
    closedir(dir);
    return found;
}

/* ------------------------------------------------------------------ */
/* The page                                                             */
/* ------------------------------------------------------------------ */

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

/* The About line: the Pack's credit, its first sentence, on paper over the
 * map's bottom edge. */
static void make_about(bool mono)
{
    const char *credit = map_pack_credit(s_ui.pack);
    char line[160];
    const char *stop = strstr(credit, ". ");
    int len = stop ? (int)(stop - credit) : (int)strlen(credit);
    snprintf(line, sizeof line, "%.*s", len, credit);

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
    lv_obj_set_style_pad_top(s_ui.about, 3, LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.about, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_ui.about, line);
    ESP_LOGI(TAG, "about: %s", credit);
}

/* ------------------------------------------------------------------ */
/* Moving                                                               */
/* ------------------------------------------------------------------ */

static void style_tracks_button(bool on);

static void shown_cb(map_view_t *view, const map_view_shown_t *shown, void *user)
{
    (void)user;
    if (s_ui.tracks) style_tracks_button(map_view_overlay_visible(view, s_ui.tracks));
    ESP_LOGI(TAG, "shown z%d at %lld,%lld: %d tiles, %d places with none, %d damaged; "
                  "%d read, %d from the cache, %u ms",
             shown->zoom, (long long)shown->left, (long long)shown->top, shown->tiles,
             shown->missing, shown->damaged, shown->read, shown->cached,
             (unsigned)shown->compose_ms);
}

static void zoom_cb(lv_event_t *e)
{
    if (s_ui.view) map_view_zoom_by(s_ui.view, (int)(intptr_t)lv_event_get_user_data(e));
}

/* ------------------------------------------------------------------ */
/* The position                                                         */
/* ------------------------------------------------------------------ */

/* The GPS fix now, if there is one. */
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
    if (s_ui.view && read_fix(&lat, &lon)) map_view_set_position(s_ui.view, lat, lon);
}

static void locate(void)
{
    if (!s_ui.view) return;
    double lat, lon;
    if (read_fix(&lat, &lon)) map_view_set_position(s_ui.view, lat, lon);
    if (map_view_locate(s_ui.view)) return;
    bool off = rs_gps_available() && !rs_gps_is_enabled();
    ESP_LOGI(TAG, "locate: %s", off ? "GPS is off" : "no fix yet");
    toast_info(off ? "GPS is off" : "No GPS fix yet");
}

static void locate_cb(lv_event_t *e)
{
    (void)e;
    locate();
}

static void toggle_tracks(void);

/* Keys the view leaves to the app. */
static void key_cb(lv_event_t *e)
{
    switch (lv_event_get_key(e)) {
    case 'l': case 'L': locate(); break;
    case 't': case 'T': toggle_tracks(); break;
    default: break;
    }
}

/* The zoom level to open the fix at: z16, or shallower where only a
 * shallower level of a Region holds it. 0 if no Region does. */
static int zoom_holding(double lat, double lon)
{
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


/* ------------------------------------------------------------------ */
/* The Navigator's tracks                                               */
/* ------------------------------------------------------------------ */

/* The number after name=" in a tag, if it has one. */
static bool tag_number(const char *tag, const char *name, double *out)
{
    size_t n = strlen(name);
    for (const char *p = tag; (p = strstr(p, name)) != NULL; p += n) {
        if (p > tag && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n' && p[-1] != '\r') continue;
        const char *q = p + n;
        while (*q == ' ') q++;
        if (*q++ != '=') continue;
        while (*q == ' ') q++;
        if (*q != '"' && *q != '\'') continue;
        char *end;
        double v = strtod(q + 1, &end);
        if (end == q + 1) return false;
        *out = v;
        return true;
    }
    return false;
}

/* One tag's text, between < and >: a trkseg starts a line, a trkpt adds a
 * point to it. Returns how many points it added (0 or 1). */
static int gpx_tag(const char *tag, bool *in_line)
{
    if (strncmp(tag, "trkseg", 6) == 0 && (tag[6] == '\0' || tag[6] == ' ' || tag[6] == '>')) {
        *in_line = map_overlay_line_begin(s_ui.tracks);
        return 0;
    }
    if (strncmp(tag, "trkpt", 5) != 0 || (tag[5] != ' ' && tag[5] != '\t' && tag[5] != '\n')) {
        return 0;
    }
    double lat, lon;
    if (!*in_line || !tag_number(tag, "lat", &lat) || !tag_number(tag, "lon", &lon)) return 0;
    return map_overlay_line_point(s_ui.tracks, lat, lon) ? 1 : 0;
}

/* Reads one GPX file's track points into the tracks overlay, a line a
 * trkseg. The file is read a chunk at a time, tag by tag. */
static int read_gpx(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char chunk[GPX_CHUNK];
    char tag[GPX_TAG_MAX];
    size_t tag_len = 0;
    bool in_tag = false, too_long = false, in_line = false;
    int points = 0;
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
        for (size_t i = 0; i < got; i++) {
            char c = chunk[i];
            if (c == '<') {
                in_tag = true;
                too_long = false;
                tag_len = 0;
            } else if (c == '>' && in_tag) {
                in_tag = false;
                if (too_long) continue;
                tag[tag_len] = '\0';
                points += gpx_tag(tag, &in_line);
            } else if (in_tag && !too_long) {
                if (tag_len + 1 >= sizeof tag) too_long = true;
                else tag[tag_len++] = c;
            }
        }
    }
    fclose(f);
    return points;
}

/* Inverted while the tracks show. Restyled only as the view showing the
 * change is swapped in, so the panel refreshes once for both. */
static void style_tracks_button(bool on)
{
    if (!s_ui.tracks_button || on == s_ui.tracks_button_on) return;
    s_ui.tracks_button_on = on;
    lv_obj_set_style_bg_color(s_ui.tracks_button, on ? lv_color_black() : lv_color_white(),
                              LV_PART_MAIN);
    /* On the label: the theme gives labels a colour of their own. */
    lv_obj_t *label = lv_obj_get_child(s_ui.tracks_button, 0);
    if (label) {
        lv_obj_set_style_text_color(label, on ? lv_color_white() : lv_color_black(),
                                    LV_PART_MAIN);
    }
}

static void toggle_tracks(void)
{
    if (!s_ui.view || !s_ui.tracks) return;
    bool on = !map_view_overlay_visible(s_ui.view, s_ui.tracks);
    map_view_set_overlay_visible(s_ui.view, s_ui.tracks, on);
    ESP_LOGI(TAG, "tracks %s", on ? "on" : "off");
}

static void tracks_cb(lv_event_t *e)
{
    (void)e;
    toggle_tracks();
}

/* Every *.gpx in TRACKS_DIR into one overlay on the view, shown. */
static void load_tracks(void)
{
    DIR *dir = opendir(TRACKS_DIR);
    if (!dir) {
        ESP_LOGI(TAG, "tracks: none in " TRACKS_DIR_SHOWN);
        return;
    }
    s_ui.tracks = map_overlay_create("tracks");
    if (!s_ui.tracks) {
        closedir(dir);
        ESP_LOGE(TAG, "tracks: no memory");
        return;
    }
    int files = 0, points = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.' || !ends_with(e->d_name, TRACK_SUFFIX)) continue;
        char path[sizeof TRACKS_DIR + NAME_MAX_LEN + 1];
        if (strlen(e->d_name) >= NAME_MAX_LEN) continue;
        snprintf(path, sizeof path, "%s/%s", TRACKS_DIR, e->d_name);
        int got = read_gpx(path);
        if (got > 0) files++;
        points += got;
    }
    closedir(dir);
    if (points == 0) {
        map_overlay_destroy(s_ui.tracks);
        s_ui.tracks = NULL;
        ESP_LOGI(TAG, "tracks: none in " TRACKS_DIR_SHOWN);
        return;
    }
    if (map_overlay_point_count(s_ui.tracks) >= MAP_OVERLAY_MAX_POINTS) {
        ESP_LOGW(TAG, "tracks: only the first %d points are shown", MAP_OVERLAY_MAX_POINTS);
    }
    map_view_add_overlay(s_ui.view, s_ui.tracks, true);
    ESP_LOGI(TAG, "tracks: %d track%s, %d points", files, files == 1 ? "" : "s", points);
}

static void make_tracks_button(void)
{
    s_ui.tracks_button = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(s_ui.tracks_button);
    lv_obj_add_style(s_ui.tracks_button, &s_button_style, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.tracks_button, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_ui.tracks_button, 8, LV_PART_MAIN);
    lv_obj_add_flag(s_ui.tracks_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ui.tracks_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_ui.tracks_button, LV_SIZE_CONTENT, ZOOM_BUTTON);
    lv_obj_align(s_ui.tracks_button, LV_ALIGN_TOP_RIGHT, -ZOOM_MARGIN, ZOOM_MARGIN);
    lv_obj_add_event_cb(s_ui.tracks_button, tracks_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(s_ui.tracks_button);
    lv_label_set_text(label, "Tracks");
    lv_obj_center(label);
    style_tracks_button(true);
}

/* A button over the map, its bottom `from_bottom` px above the map's
 * bottom edge, at the right (moved after, for one at the left). */
static lv_obj_t *make_button(const char *text, lv_event_cb_t cb, void *user, int from_bottom)
{
    if (!s_button_style_ready) {
        lv_style_init(&s_button_style);
        lv_style_set_bg_color(&s_button_style, lv_color_white());
        lv_style_set_bg_opa(&s_button_style, LV_OPA_COVER);
        lv_style_set_border_color(&s_button_style, lv_color_black());
        lv_style_set_border_width(&s_button_style, 2);
        lv_style_set_radius(&s_button_style, 4);
        lv_style_set_text_color(&s_button_style, lv_color_black());
        lv_style_set_text_font(&s_button_style, &lv_font_montserrat_18);
        s_button_style_ready = true;
    }
    lv_obj_t *button = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(button);
    lv_obj_add_style(button, &s_button_style, LV_PART_MAIN);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(button, ZOOM_BUTTON, ZOOM_BUTTON);
    lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, -ZOOM_MARGIN, -from_bottom);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

static void log_centre(const char *what, lv_obj_t *obj)
{
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    ESP_LOGI(TAG, "%s at %d,%d", what, (int)(a.x1 + a.x2) / 2, (int)(a.y1 + a.y2) / 2);
}

/* Where a button covers the map, for a test to look past it. */
static void log_button(const char *what, lv_obj_t *obj)
{
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    ESP_LOGI(TAG, "%s button %d,%d-%d,%d", what, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2);
}

static void open_and_show(bool mono)
{
    char name[NAME_MAX_LEN];
    if (!find_pack(name, sizeof name)) {
        ESP_LOGI(TAG, "no Map Pack in " MAPS_DIR_SHOWN);
        show_message("No map yet",
                     "Build a Map Pack with the Map Pack tool and copy it to " MAPS_DIR_SHOWN "/");
        return;
    }

    char path[sizeof MAPS_DIR + NAME_MAX_LEN + 1];
    snprintf(path, sizeof path, "%s/%s", MAPS_DIR, name);
    unsigned newer = 0;
    map_pack_err_t err = map_pack_open(path, &s_ui.pack, &newer);
    if (err != MAP_PACK_OK) {
        char why[160];
        if (err == MAP_PACK_ERR_NEWER) {
            snprintf(why, sizeof why, "%s (format %u)", map_pack_err_text(err), newer);
        } else {
            snprintf(why, sizeof why, "%s", map_pack_err_text(err));
        }
        ESP_LOGW(TAG, "cannot open %s: %s", name, why);
        char detail[300];
        snprintf(detail, sizeof detail, "%s is %s.%s", name, why,
                 err == MAP_PACK_ERR_NEWER ? " Update ThistleOS to read it." : "");
        show_message("Cannot show this map", detail);
        return;
    }
    ESP_LOGI(TAG, "opened %s: format %u.%u, %d regions, %u tiles", name,
             map_pack_version_major(s_ui.pack), map_pack_version_minor(s_ui.pack),
             map_pack_region_count(s_ui.pack), (unsigned)map_pack_tile_count(s_ui.pack));

    s_ui.view = map_view_create(s_ui.root, s_ui.pack, shown_cb, NULL);
    if (!s_ui.view) {
        ESP_LOGE(TAG, "no memory for the map");
        show_message("Cannot show this map", "There is not enough memory for the map.");
        return;
    }

    map_view_set_bottom_inset(s_ui.view, ABOUT_H);

    /* The fix, if a Region holds it; else the first Region's centre. The
     * Pack is read here before the view's task reads it: nothing is asked
     * of the view yet. */
    double lat, lon;
    int zoom = 0;
    if (read_fix(&lat, &lon)) {
        map_view_set_position(s_ui.view, lat, lon);
        zoom = zoom_holding(lat, lon);
    }
    if (zoom) {
        map_view_go_to(s_ui.view, lat, lon, zoom);
        ESP_LOGI(TAG, "showing z%d around %.7f, %.7f (your position)", zoom, lat, lon);
    } else {
        const map_region_t *first = map_pack_region(s_ui.pack, 0);
        zoom = first->deepest_zoom < DEFAULT_ZOOM ? first->deepest_zoom : DEFAULT_ZOOM;
        map_view_go_to(s_ui.view, first->centre_lat, first->centre_lon, zoom);
        ESP_LOGI(TAG, "showing z%d around %.7f, %.7f (%s)", zoom, first->centre_lat,
                 first->centre_lon, first->name);
    }
    s_ui.fix_poll = lv_timer_create(fix_poll_cb, FIX_POLL_MS, NULL);
    lv_obj_add_event_cb(map_view_obj(s_ui.view), key_cb, LV_EVENT_KEY, NULL);

    lv_obj_t *out = make_button("-", zoom_cb, (void *)(intptr_t)-1, ABOUT_H + ZOOM_MARGIN);
    lv_obj_t *in = make_button("+", zoom_cb, (void *)(intptr_t)1,
                               ABOUT_H + ZOOM_MARGIN * 2 + ZOOM_BUTTON);
    /* Locate at the left, clear of a swipe's start and the zoom column. */
    lv_obj_t *loc = make_button(LV_SYMBOL_GPS, locate_cb, NULL, ABOUT_H + ZOOM_MARGIN);
    lv_obj_align(loc, LV_ALIGN_BOTTOM_LEFT, ZOOM_MARGIN, -(ABOUT_H + ZOOM_MARGIN));
    load_tracks();
    if (s_ui.tracks) make_tracks_button();
    make_about(mono);

    lv_obj_update_layout(s_ui.root);
    lv_area_t at;
    lv_obj_get_coords(map_view_obj(s_ui.view), &at);
    ESP_LOGI(TAG, "view %dx%d at %d,%d", map_view_width(s_ui.view), map_view_height(s_ui.view),
             (int)at.x1, (int)at.y1);
    log_centre("zoom in", in);
    log_centre("zoom out", out);
    log_button("locate", loc);
    if (s_ui.tracks_button) log_button("tracks", s_ui.tracks_button);
}

esp_err_t maps_ui_create(lv_obj_t *parent)
{
    if (!parent) parent = lv_screen_active();
    memset(&s_ui, 0, sizeof s_ui);

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

    open_and_show(mono);
    return ESP_OK;
}

void maps_ui_destroy(void)
{
    if (s_ui.fix_poll) lv_timer_delete(s_ui.fix_poll);
    map_view_destroy(s_ui.view);
    if (s_ui.root) lv_obj_delete(s_ui.root);
    map_overlay_destroy(s_ui.tracks);  /* after the view, which reads it */
    map_pack_close(s_ui.pack);
    memset(&s_ui, 0, sizeof s_ui);
}
