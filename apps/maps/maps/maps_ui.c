/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Maps screen
 *
 *   ┌──────────────────────────────┐
 *   │                              │
 *   │     the Map Pack's tiles     │  opens on the first Region's centre,
 *   │         (map_engine)         │  at z16 (or the Region's deepest
 *   │                        [ + ] │  level, if shallower)
 *   │                        [ - ] │  zoom in and out, a level a tap
 *   ├──────────────────────────────┤
 *   │ © OpenStreetMap contributors │  the About line: the Pack's credit
 *   └──────────────────────────────┘
 *
 * The Pack is the first *.tmap, by name, in /sdcard/maps/. Without one, or
 * with one that does not open, the page says why instead.
 *
 * A swipe or the keys pan half a screen; the zoom buttons and keys go
 * through the four levels (map_view.h). Every GeoJSON file in
 * /sdcard/maps/overlays/ is a switchable overlay, with an info card for a
 * tapped item (maps_layers.c).
 */
#include "maps/maps_app.h"
#include "maps/maps_layers.h"

#include "map_engine/map_pack.h"
#include "map_engine/map_view.h"

#include "hal/board.h"
#include "hal/sdcard_path.h"
#include "ui/theme.h"

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

/* The Reader's font, Latin-1 with it, for the credit's ©, which Montserrat
 * lacks: the Podcasts app borrows it the same way. */
LV_FONT_DECLARE(reader_font_16);
LV_FONT_DECLARE(reader_font_16_mono);

static struct {
    lv_obj_t   *root;
    lv_obj_t   *about;
    map_view_t *view;
    map_pack_t *pack;
} s_ui;

/* The zoom buttons' look: shared, and with no pressed state, as a press
 * that drew would be a refresh of its own on e-paper. */
static lv_style_t s_zoom_style;
static bool s_zoom_style_ready;

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

static void shown_cb(map_view_t *view, const map_view_shown_t *shown, void *user)
{
    (void)view;
    (void)user;
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

/* A zoom button, its bottom `from_bottom` px above the map's bottom edge. */
static lv_obj_t *make_zoom_button(const char *text, int levels, int from_bottom)
{
    if (!s_zoom_style_ready) {
        lv_style_init(&s_zoom_style);
        lv_style_set_bg_color(&s_zoom_style, lv_color_white());
        lv_style_set_bg_opa(&s_zoom_style, LV_OPA_COVER);
        lv_style_set_border_color(&s_zoom_style, lv_color_black());
        lv_style_set_border_width(&s_zoom_style, 2);
        lv_style_set_radius(&s_zoom_style, 4);
        lv_style_set_text_color(&s_zoom_style, lv_color_black());
        lv_style_set_text_font(&s_zoom_style, &lv_font_montserrat_18);
        s_zoom_style_ready = true;
    }
    lv_obj_t *button = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(button);
    lv_obj_add_style(button, &s_zoom_style, LV_PART_MAIN);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(button, ZOOM_BUTTON, ZOOM_BUTTON);
    lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, -ZOOM_MARGIN, -from_bottom);
    lv_obj_add_event_cb(button, zoom_cb, LV_EVENT_CLICKED, (void *)(intptr_t)levels);
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

    /* Until there is a position: the first Region's centre. */
    const map_region_t *first = map_pack_region(s_ui.pack, 0);
    int zoom = first->deepest_zoom < DEFAULT_ZOOM ? first->deepest_zoom : DEFAULT_ZOOM;
    map_view_go_to(s_ui.view, first->centre_lat, first->centre_lon, zoom);
    ESP_LOGI(TAG, "showing z%d around %.7f, %.7f (%s)", zoom, first->centre_lat,
             first->centre_lon, first->name);

    lv_obj_t *out = make_zoom_button("-", -1, ABOUT_H + ZOOM_MARGIN);
    lv_obj_t *in = make_zoom_button("+", 1, ABOUT_H + ZOOM_MARGIN * 2 + ZOOM_BUTTON);
    make_about(mono);
    maps_layers_open(s_ui.view, s_ui.root, ABOUT_H);

    lv_obj_update_layout(s_ui.root);
    lv_area_t at;
    lv_obj_get_coords(map_view_obj(s_ui.view), &at);
    ESP_LOGI(TAG, "view %dx%d at %d,%d", map_view_width(s_ui.view), map_view_height(s_ui.view),
             (int)at.x1, (int)at.y1);
    log_centre("zoom in", in);
    log_centre("zoom out", out);
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
    if (s_ui.view) maps_layers_close();
    map_view_destroy(s_ui.view);
    if (s_ui.root) lv_obj_delete(s_ui.root);
    map_pack_close(s_ui.pack);
    memset(&s_ui, 0, sizeof s_ui);
}
