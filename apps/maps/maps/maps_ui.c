/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Maps screen
 *
 *   ┌──────────────────────────────┐
 *   │                              │
 *   │     the Map Pack's tiles     │  the first Region's centre in the
 *   │         (map_engine)         │  middle, at z16 (or the Region's
 *   │                              │  deepest level, if shallower)
 *   │                              │
 *   ├──────────────────────────────┤
 *   │ © OpenStreetMap contributors │  the About line: the Pack's credit
 *   └──────────────────────────────┘
 *
 * The Pack is the first *.tmap, by name, in /sdcard/maps/. Without one, or
 * with one that does not open, the page says why instead.
 *
 * For now the map stays where it opens; pan, zoom, the position marker and
 * overlays are the next tickets' (.scratch/maps/).
 */
#include "maps/maps_app.h"

#include "map_engine/map_pack.h"
#include "map_engine/map_view.h"

#include "hal/board.h"
#include "hal/sdcard_path.h"
#include "ui/theme.h"

#include "esp_log.h"
#include "lvgl.h"

#include <dirent.h>
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

    s_ui.view = map_view_create(s_ui.root);
    if (!s_ui.view) {
        ESP_LOGE(TAG, "no memory for the map");
        show_message("Cannot show this map", "There is not enough memory for the map.");
        return;
    }

    /* A fixed position for now: the first Region's centre. */
    const map_region_t *first = map_pack_region(s_ui.pack, 0);
    int zoom = first->deepest_zoom < DEFAULT_ZOOM ? first->deepest_zoom : DEFAULT_ZOOM;
    map_view_stats_t drawn = map_view_show(s_ui.view, s_ui.pack, first->centre_lat,
                                           first->centre_lon, zoom);
    ESP_LOGI(TAG, "showing z%d around %.7f, %.7f (%s)", zoom, first->centre_lat,
             first->centre_lon, first->name);
    ESP_LOGI(TAG, "drew %d tiles, %d places with none, %d damaged", drawn.tiles,
             drawn.missing, drawn.damaged);

    make_about(mono);

    lv_obj_update_layout(s_ui.root);
    lv_area_t at;
    lv_obj_get_coords(map_view_obj(s_ui.view), &at);
    ESP_LOGI(TAG, "view %dx%d at %d,%d", map_view_width(s_ui.view), map_view_height(s_ui.view),
             (int)at.x1, (int)at.y1);
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
    map_view_destroy(s_ui.view);
    if (s_ui.root) lv_obj_delete(s_ui.root);
    map_pack_close(s_ui.pack);
    memset(&s_ui, 0, sizeof s_ui);
}
