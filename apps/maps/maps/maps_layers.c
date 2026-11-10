/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Maps app's overlays: the Pack's own, and the GeoJSON
 * files on the card
 *
 *   ┌──────────────────────────────┐
 *   │ [≡]                     [⟳]  │  Layers: each file on or off.
 *   │ ┌──────────┐                 │  Refresh: the files read again if
 *   │ │ [x] pois │                 │  they changed, and drawn at once
 *   │ │ [ ] live │                 │
 *   │ └──────────┘    • Fountain   │
 *   │ ┌──────────────────────────┐ │  a tap near an item: its card,
 *   │ │ Fountain                 │ │  its name and properties; a tap on
 *   │ │ kind: water              │ │  the card, or on nothing, closes it
 *   │ └──────────────────────────┘ │
 *   └──────────────────────────────┘
 *
 * The overlays baked into the Map Pack (map_pack.h), a static transit
 * network's lines and stops, come first in the panel, beneath the files'
 * on the map, with an info card of their own properties.
 *
 * Every *.geojson and *.json in /sdcard/maps/overlays/, by name, is an
 * overlay (map_overlay.h), named by its file's name without the suffix,
 * each above the one before; at most MAX_FILES. With no overlay at all
 * there is no Layers button, and with no file no Refresh. A file is a GeoJSON
 * FeatureCollection: Points and MultiPoints are points, labelled with
 * their "name" (or "title"), with the icon their "marker-symbol" (or
 * "icon") names: circle, square, triangle or cross, else a dot, and
 * "text" for a label alone; LineStrings and MultiLineStrings are lines;
 * Polygons and MultiPolygons their outer rings. An overlay holds 256
 * items; the rest of a file is left out, and logged.
 *
 * A file that changes on the card, as a Provider writing its last result
 * does, is read again within WATCH_MS, and drawn as the overlay policy
 * says: at most every 20 s, at once on Refresh (r).
 *
 * The file's text stays in PSRAM while the app is open, so a card reads
 * its item's properties from it.
 */
#include "maps/maps_layers.h"

#include "map_engine/map_overlay.h"
#include "podcasts/pod_json.h"

#include "hal/sdcard_path.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "maps";

#define OVERLAYS_DIR  THISTLE_SDCARD "/maps/overlays"
#define MAX_FILES     8
#define MAX_LAYERS    (MAX_FILES + MAP_PACK_MAX_OVERLAYS)
#define FILE_NAME_MAX 64
#define FILE_MAX_SIZE (1024 * 1024)
#define WATCH_MS      2000
#define BUTTON        36
#define MARGIN        6
#define CARD_LINES    10
#define CARD_TEXT     640
#define VALUE_MAX     160

typedef struct {
    uint32_t at, len;  /* the feature's "properties" in the text; len 0: none */
} props_t;

typedef struct {
    int            pack_overlay;  /* the Pack's overlay; -1 for a file */
    char           file[FILE_NAME_MAX];
    char           name[MAP_OVERLAY_NAME_MAX];
    time_t         mtime;
    off_t          size;
    map_overlay_t *overlay;
    char          *text;   /* the file, PSRAM */
    size_t         len;
    props_t        props[MAP_OVERLAY_MAX_ITEMS];  /* by item */
    lv_obj_t      *row;    /* in the Layers panel, while open */
} layer_t;

static struct {
    map_view_t *view;
    lv_obj_t   *root;
    int         bottom;
    map_pack_t *pack;
    layer_t    *layers;  /* MAX_LAYERS, PSRAM: the Pack's, then the files */
    int         count;
    int         files;
    lv_timer_t *watch;
    lv_obj_t   *layers_button;
    lv_obj_t   *panel;
    lv_obj_t   *card;
    map_lat_lon_t *points;  /* MAP_OVERLAY_MAX_VERTICES, while reading */
} s;

static lv_style_t s_button_style;
static lv_style_t s_box_style;
static bool s_styles_ready;

static void *alloc_psram(size_t len)
{
    void *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(len);
}

/* ------------------------------------------------------------------ */
/* Reading GeoJSON                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    layer_t *layer;
    int      added;
    int      left_out;
    bool     bad;
} reading_t;

static bool read_number(pod_json_t *j, double *out)
{
    char buf[40];
    if (!pod_json_string(j, buf, sizeof buf) || !buf[0]) return false;
    char *end;
    *out = strtod(buf, &end);
    return end != buf;
}

/* A position, [lon, lat, ...]. */
static bool read_position(pod_json_t *j, map_lat_lon_t *out)
{
    if (!pod_json_array(j)) return false;
    double v[2];
    int n = 0;
    while (pod_json_next(j)) {
        double d;
        if (n < 2 && read_number(j, &d)) v[n++] = d;
        else pod_json_skip(j);
    }
    if (n < 2 || v[1] < -90 || v[1] > 90 || v[0] < -180 || v[0] > 180) return false;
    out->lon = v[0];
    out->lat = v[1];
    return true;
}

/* An array of positions into s.points; how many. */
static int read_positions(pod_json_t *j)
{
    int n = 0;
    if (!pod_json_array(j)) return 0;
    while (pod_json_next(j)) {
        map_lat_lon_t p;
        if (read_position(j, &p) && n < MAP_OVERLAY_MAX_VERTICES) s.points[n++] = p;
    }
    return n;
}

typedef struct {
    char       label[MAP_OVERLAY_LABEL_MAX];
    map_icon_t icon;
    bool       text_only;
} style_t;

static void count_added(reading_t *r, int item, props_t props)
{
    if (item < 0) {
        r->left_out++;
        return;
    }
    r->layer->props[item] = props;
    r->added++;
}

static void add_point(reading_t *r, const map_lat_lon_t *p, const style_t *st, props_t props)
{
    map_overlay_t *o = r->layer->overlay;
    int item = st->text_only && st->label[0]
                   ? map_overlay_add_text(o, p->lat, p->lon, st->label, 0)
                   : map_overlay_add_point(o, p->lat, p->lon, st->icon, st->label, 0);
    count_added(r, item, props);
}

/* A Polygon's rings: the outer one is the polygon. */
static void read_polygon(pod_json_t *j, reading_t *r, props_t props)
{
    if (!pod_json_array(j)) return;
    bool outer = true;
    while (pod_json_next(j)) {
        if (!outer) {
            pod_json_skip(j);
            continue;
        }
        outer = false;
        int n = read_positions(j);
        if (n >= 2 && s.points[0].lat == s.points[n - 1].lat &&
            s.points[0].lon == s.points[n - 1].lon) {
            n--;  /* GeoJSON closes its rings; the overlay closes them itself */
        }
        count_added(r, map_overlay_add_polygon(r->layer->overlay, s.points, n, 0), props);
    }
}

static void read_geometry(const char *at, const char *end, reading_t *r, const style_t *st,
                          props_t props)
{
    pod_json_t j;
    pod_json_init(&j, at, (size_t)(end - at));
    char key[16], type[24] = "";
    const char *coords = NULL;
    if (!pod_json_object(&j)) return;
    while (pod_json_key(&j, key, sizeof key)) {
        if (strcmp(key, "type") == 0) {
            pod_json_string(&j, type, sizeof type);
        } else if (strcmp(key, "coordinates") == 0) {
            coords = j.p;
            pod_json_skip(&j);
        } else {
            pod_json_skip(&j);
        }
    }
    if (j.error || !coords) return;
    pod_json_t c;
    pod_json_init(&c, coords, (size_t)(end - coords));
    map_overlay_t *o = r->layer->overlay;
    if (strcmp(type, "Point") == 0) {
        map_lat_lon_t p;
        if (read_position(&c, &p)) add_point(r, &p, st, props);
    } else if (strcmp(type, "MultiPoint") == 0) {
        int n = read_positions(&c);
        for (int i = 0; i < n; i++) add_point(r, &s.points[i], st, props);
    } else if (strcmp(type, "LineString") == 0) {
        int n = read_positions(&c);
        count_added(r, map_overlay_add_line(o, s.points, n, 0), props);
    } else if (strcmp(type, "MultiLineString") == 0) {
        if (pod_json_array(&c)) {
            while (pod_json_next(&c)) {
                int n = read_positions(&c);
                count_added(r, map_overlay_add_line(o, s.points, n, 0), props);
            }
        }
    } else if (strcmp(type, "Polygon") == 0) {
        read_polygon(&c, r, props);
    } else if (strcmp(type, "MultiPolygon") == 0) {
        if (pod_json_array(&c)) {
            while (pod_json_next(&c)) read_polygon(&c, r, props);
        }
    }
}

static map_icon_t icon_named(const char *name, bool *text_only)
{
    *text_only = strcmp(name, "text") == 0;
    if (strcmp(name, "circle") == 0 || strcmp(name, "circle-stroked") == 0) return MAP_ICON_CIRCLE;
    if (strcmp(name, "square") == 0 || strcmp(name, "square-stroked") == 0) return MAP_ICON_SQUARE;
    if (strcmp(name, "triangle") == 0 || strcmp(name, "triangle-stroked") == 0) {
        return MAP_ICON_TRIANGLE;
    }
    if (strcmp(name, "cross") == 0 || strcmp(name, "hospital") == 0) return MAP_ICON_CROSS;
    return MAP_ICON_DOT;
}

/* A feature's properties: its label and icon. */
static void read_style(const char *at, const char *end, style_t *st)
{
    memset(st, 0, sizeof *st);
    pod_json_t j;
    pod_json_init(&j, at, (size_t)(end - at));
    char key[24], value[MAP_OVERLAY_LABEL_MAX], title[MAP_OVERLAY_LABEL_MAX] = "";
    if (!pod_json_object(&j)) return;
    while (pod_json_key(&j, key, sizeof key)) {
        if (strcmp(key, "name") == 0) {
            pod_json_string(&j, st->label, sizeof st->label);
        } else if (strcmp(key, "title") == 0) {
            pod_json_string(&j, title, sizeof title);
        } else if (strcmp(key, "marker-symbol") == 0 || strcmp(key, "icon") == 0) {
            pod_json_string(&j, value, sizeof value);
            st->icon = icon_named(value, &st->text_only);
        } else {
            pod_json_skip(&j);
        }
    }
    if (!st->label[0]) snprintf(st->label, sizeof st->label, "%s", title);
}

static void read_feature(pod_json_t *j, reading_t *r, const char *end)
{
    if (!pod_json_object(j)) return;
    char key[16];
    const char *geometry = NULL;
    props_t props = {0, 0};
    while (pod_json_key(j, key, sizeof key)) {
        if (strcmp(key, "geometry") == 0) {
            geometry = j->p;
            pod_json_skip(j);
        } else if (strcmp(key, "properties") == 0) {
            const char *from = j->p;
            pod_json_skip(j);
            props.at = (uint32_t)(from - r->layer->text);
            props.len = (uint32_t)(j->p - from);
        } else {
            pod_json_skip(j);
        }
    }
    if (j->error || !geometry) return;
    style_t st;
    if (props.len) read_style(r->layer->text + props.at, r->layer->text + props.at + props.len, &st);
    else memset(&st, 0, sizeof st);
    read_geometry(geometry, end, r, &st, props);
}

/* The layer's text, a FeatureCollection, into its overlay. False if it is
 * not GeoJSON. */
static bool read_collection(layer_t *layer, reading_t *r)
{
    const char *end = layer->text + layer->len;
    pod_json_t j;
    pod_json_init(&j, layer->text, layer->len);
    char key[16], type[24] = "";
    bool features = false;
    if (!pod_json_object(&j)) return false;
    while (pod_json_key(&j, key, sizeof key)) {
        if (strcmp(key, "type") == 0) {
            pod_json_string(&j, type, sizeof type);
        } else if (strcmp(key, "features") == 0 && pod_json_array(&j)) {
            features = true;
            while (pod_json_next(&j)) read_feature(&j, r, end);
        } else {
            pod_json_skip(&j);
        }
    }
    return !j.error && features && strcmp(type, "FeatureCollection") == 0;
}

/* Reads the layer's file into its overlay, which is cleared first. */
static void load(layer_t *layer, const struct stat *st)
{
    map_overlay_clear(layer->overlay);
    free(layer->text);
    layer->text = NULL;
    layer->len = 0;
    layer->mtime = st->st_mtime;
    layer->size = st->st_size;
    memset(layer->props, 0, sizeof layer->props);

    char path[sizeof OVERLAYS_DIR + FILE_NAME_MAX + 1];
    snprintf(path, sizeof path, "%s/%s", OVERLAYS_DIR, layer->file);
    if (st->st_size > FILE_MAX_SIZE) {
        ESP_LOGW(TAG, "overlay %s: cannot read it: larger than %d KB", layer->file,
                 FILE_MAX_SIZE / 1024);
        return;
    }
    FILE *f = fopen(path, "rb");
    layer->text = alloc_psram((size_t)st->st_size + 1);
    if (!f || !layer->text) {
        if (f) fclose(f);
        ESP_LOGW(TAG, "overlay %s: cannot read it", layer->file);
        return;
    }
    layer->len = fread(layer->text, 1, (size_t)st->st_size, f);
    layer->text[layer->len] = '\0';
    fclose(f);

    reading_t r = {.layer = layer};
    if (!read_collection(layer, &r)) {
        map_overlay_clear(layer->overlay);
        ESP_LOGW(TAG, "overlay %s: cannot read it: not GeoJSON", layer->file);
        return;
    }
    if (r.left_out) {
        ESP_LOGI(TAG, "overlay %s: %d items, %d left out (%d at most)", layer->file, r.added,
                 r.left_out, MAP_OVERLAY_MAX_ITEMS);
    } else {
        ESP_LOGI(TAG, "overlay %s: %d items", layer->file, r.added);
    }
}

/* ------------------------------------------------------------------ */
/* The info card                                                        */
/* ------------------------------------------------------------------ */

static void close_card(void)
{
    if (!s.card) return;
    lv_obj_delete(s.card);
    s.card = NULL;
    ESP_LOGI(TAG, "info card closed");
}

static void card_cb(lv_event_t *e)
{
    (void)e;
    close_card();
}

/* "key: value" lines of a feature's properties, but its name. */
static void card_lines(const layer_t *layer, props_t props, char *out, size_t cap, char *log,
                       size_t log_cap)
{
    out[0] = '\0';
    if (!props.len) return;
    pod_json_t j;
    pod_json_init(&j, layer->text + props.at, props.len);
    char key[32], value[VALUE_MAX];
    int lines = 0;
    size_t used = 0, logged = strlen(log);
    if (!pod_json_object(&j)) return;
    while (pod_json_key(&j, key, sizeof key)) {
        if (strcmp(key, "name") == 0 || strcmp(key, "marker-symbol") == 0 ||
            strcmp(key, "icon") == 0 || lines >= CARD_LINES) {
            pod_json_skip(&j);
            continue;
        }
        if (!pod_json_string(&j, value, sizeof value) || !value[0]) continue;
        int n = snprintf(out + used, cap - used, "%s%s: %s", lines ? "\n" : "", key, value);
        if (n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
        lines++;
        int m = snprintf(log + logged, log_cap - logged, "; %s: %s", key, value);
        if (m > 0 && (size_t)m < log_cap - logged) logged += (size_t)m;
    }
}

/* The same for an item of the Pack's: its properties as the Pack has them. */
static void pack_card_lines(const layer_t *layer, int id, char *out, size_t cap, char *log,
                            size_t log_cap)
{
    out[0] = '\0';
    size_t used = 0, logged = strlen(log);
    char key[MAP_PACK_KEY_MAX], value[MAP_PACK_VALUE_MAX];
    for (int p = 0; p < CARD_LINES && map_pack_overlay_prop(s.pack, layer->pack_overlay, id, p, key,
                                                            sizeof key, value, sizeof value);
         p++) {
        if (!value[0]) continue;
        int n = snprintf(out + used, cap - used, "%s%s: %s", used ? "\n" : "", key, value);
        if (n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
        int m = snprintf(log + logged, log_cap - logged, "; %s: %s", key, value);
        if (m > 0 && (size_t)m < log_cap - logged) logged += (size_t)m;
    }
}

static void show_card(layer_t *layer, int item, uint32_t id)
{
    close_card();
    /* A Pack's line has a label the overlay does not draw: its routes. */
    map_pack_item_t pi;
    const char *name = map_overlay_item_label(layer->overlay, item);
    if (layer->pack_overlay >= 0 &&
        map_pack_overlay_item(s.pack, layer->pack_overlay, (int)id, &pi)) {
        name = pi.label;
    }
    char title[MAP_OVERLAY_LABEL_MAX + 16];
    if (name[0]) snprintf(title, sizeof title, "%s", name);
    else snprintf(title, sizeof title, "%s", layer->name);
    static char body[CARD_TEXT];
    char log[CARD_TEXT + 96];
    snprintf(log, sizeof log, "%s", title);
    if (layer->pack_overlay >= 0) pack_card_lines(layer, (int)id, body, sizeof body, log, sizeof log);
    else card_lines(layer, layer->props[item], body, sizeof body, log, sizeof log);
    ESP_LOGI(TAG, "info card: %s", log);

    s.card = lv_obj_create(s.root);
    lv_obj_remove_style_all(s.card);
    lv_obj_add_style(s.card, &s_box_style, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s.card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s.card, 2, LV_PART_MAIN);
    lv_obj_set_size(s.card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(s.card, 150, LV_PART_MAIN);
    lv_obj_set_flex_flow(s.card, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(s.card, LV_ALIGN_BOTTOM_MID, 0, -s.bottom);
    lv_obj_add_flag(s.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s.card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s.card, card_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = lv_label_create(s.card);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_font(t, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_label_set_text(t, title);
    if (body[0]) {
        lv_obj_t *b = lv_label_create(s.card);
        lv_obj_set_width(b, lv_pct(100));
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
        lv_label_set_text_static(b, body);
    }
    lv_obj_update_layout(s.card);
    lv_area_t a;
    lv_obj_get_coords(s.card, &a);
    ESP_LOGI(TAG, "info card shown at %d,%d", (int)(a.x1 + a.x2) / 2, (int)(a.y1 + a.y2) / 2);
}

static void tap_cb(map_overlay_t *overlay, int item, uint32_t id, void *user)
{
    (void)overlay;
    show_card(user, item, id);
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
/* Watching, Refresh and the Layers panel                               */
/* ------------------------------------------------------------------ */

/* Reads again each file that changed; whether any did. */
static bool reread_changed(void)
{
    bool any = false;
    for (int i = 0; i < s.count; i++) {
        layer_t *l = &s.layers[i];
        if (l->pack_overlay >= 0) continue;
        char path[sizeof OVERLAYS_DIR + FILE_NAME_MAX + 1];
        snprintf(path, sizeof path, "%s/%s", OVERLAYS_DIR, l->file);
        struct stat st;
        if (stat(path, &st) != 0 || (st.st_mtime == l->mtime && st.st_size == l->size)) continue;
        ESP_LOGI(TAG, "overlay %s changed on the card", l->file);
        if (s.card) close_card();  /* its item may be gone */
        load(l, &st);
        map_overlay_changed(l->overlay);
        any = true;
    }
    return any;
}

static void watch_cb(lv_timer_t *t)
{
    (void)t;
    reread_changed();
}

static void refresh(void)
{
    ESP_LOGI(TAG, "refresh");
    reread_changed();
    map_view_refresh(s.view);
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    refresh();
}

static void row_text(lv_obj_t *label, const layer_t *l)
{
    lv_label_set_text_fmt(label, "[%s] %s", map_overlay_visible(l->overlay) ? "x" : "  ",
                          l->name);
}

static void row_cb(lv_event_t *e)
{
    layer_t *l = lv_event_get_user_data(e);
    bool on = !map_overlay_visible(l->overlay);
    ESP_LOGI(TAG, "overlay %s %s", l->name, on ? "on" : "off");
    map_overlay_set_visible(l->overlay, on);
    row_text(lv_obj_get_child(l->row, 0), l);
}

static void close_panel(void)
{
    if (!s.panel) return;
    lv_obj_delete(s.panel);
    s.panel = NULL;
    for (int i = 0; i < s.count; i++) s.layers[i].row = NULL;
}

static void open_panel(void)
{
    close_card();
    s.panel = lv_obj_create(s.root);
    lv_obj_remove_style_all(s.panel);
    lv_obj_add_style(s.panel, &s_box_style, LV_PART_MAIN);
    lv_obj_set_size(s.panel, 170, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s.panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(s.panel, LV_ALIGN_TOP_LEFT, MARGIN, MARGIN * 2 + BUTTON);
    lv_obj_remove_flag(s.panel, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < s.count; i++) {
        layer_t *l = &s.layers[i];
        l->row = lv_obj_create(s.panel);
        lv_obj_remove_style_all(l->row);
        lv_obj_set_size(l->row, lv_pct(100), 30);
        lv_obj_set_style_pad_hor(l->row, 6, LV_PART_MAIN);
        lv_obj_add_flag(l->row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(l->row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(l->row, row_cb, LV_EVENT_CLICKED, l);
        lv_obj_t *label = lv_label_create(l->row);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, lv_pct(100));
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
        row_text(label, l);
    }
    lv_obj_update_layout(s.panel);
    for (int i = 0; i < s.count; i++) {
        lv_area_t a;
        lv_obj_get_coords(s.layers[i].row, &a);
        ESP_LOGI(TAG, "layer %s at %d,%d", s.layers[i].name, (int)(a.x1 + a.x2) / 2,
                 (int)(a.y1 + a.y2) / 2);
    }
}

static void layers_cb(lv_event_t *e)
{
    (void)e;
    if (s.panel) close_panel();
    else open_panel();
}

static void key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == 'r' || key == 'R') refresh();
    else if (key == LV_KEY_ESC) {
        close_card();
        close_panel();
    }
}

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
    lv_style_init(&s_box_style);
    lv_style_set_bg_color(&s_box_style, lv_color_white());
    lv_style_set_bg_opa(&s_box_style, LV_OPA_COVER);
    lv_style_set_border_color(&s_box_style, lv_color_black());
    lv_style_set_border_width(&s_box_style, 2);
    lv_style_set_text_color(&s_box_style, lv_color_black());
    s_styles_ready = true;
}

/* A button with a symbol, in a top corner. */
static lv_obj_t *corner_button(const char *symbol, lv_align_t align, int x, lv_event_cb_t cb,
                               const char *what)
{
    lv_obj_t *b = lv_obj_create(s.root);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &s_button_style, LV_PART_MAIN);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(b, BUTTON, BUTTON);
    lv_obj_align(b, align, x, MARGIN);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(b);
    lv_label_set_text(label, symbol);
    lv_obj_center(label);
    lv_obj_update_layout(b);
    lv_area_t a;
    lv_obj_get_coords(b, &a);
    ESP_LOGI(TAG, "%s at %d,%d", what, (int)(a.x1 + a.x2) / 2, (int)(a.y1 + a.y2) / 2);
    return b;
}

/* ------------------------------------------------------------------ */
/* Opening                                                              */
/* ------------------------------------------------------------------ */

static bool is_overlay_file(const char *name)
{
    size_t n = strlen(name);
    if (name[0] == '.' || n >= FILE_NAME_MAX) return false;
    return (n > 8 && strcmp(name + n - 8, ".geojson") == 0) ||
           (n > 5 && strcmp(name + n - 5, ".json") == 0);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const layer_t *)a)->file, ((const layer_t *)b)->file);
}

/* The overlay files' names, sorted, into s.layers after the Pack's. */
static void find_files(void)
{
    layer_t *first = &s.layers[s.count];
    DIR *dir = opendir(OVERLAYS_DIR);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (!is_overlay_file(e->d_name)) continue;
        if (s.files == MAX_FILES) {
            ESP_LOGW(TAG, "more than %d overlay files: %s and the rest left out", MAX_FILES,
                     e->d_name);
            break;
        }
        layer_t *l = &first[s.files++];
        memset(l, 0, sizeof *l);
        l->pack_overlay = -1;
        snprintf(l->file, sizeof l->file, "%s", e->d_name);
    }
    closedir(dir);
    qsort(first, (size_t)s.files, sizeof *s.layers, by_name);
}

/* The Pack's overlays, each a layer. */
static void open_pack_overlays(map_view_t *view)
{
    if (map_pack_overlays_damaged(s.pack)) {
        ESP_LOGW(TAG, "the Pack's overlays are damaged: left out");
    }
    for (int o = 0; o < map_pack_overlay_count(s.pack); o++) {
        layer_t *l = &s.layers[s.count];
        memset(l, 0, sizeof *l);
        l->pack_overlay = o;
        snprintf(l->name, sizeof l->name, "%s", map_pack_overlay_name(s.pack, o));
        /* Beneath the files', above the tracks (made before, at 0). */
        l->overlay = map_overlay_create(view, l->name, 0, tap_cb, l);
        if (!l->overlay) {
            ESP_LOGE(TAG, "no memory for the Pack's overlay %s", l->name);
            return;
        }
        s.count++;
        int added = map_overlay_add_from_pack(l->overlay, s.pack, o);
        int items = map_pack_overlay_items(s.pack, o);
        if (added < items) {
            ESP_LOGW(TAG, "Pack overlay %s: %d items, %d left out", l->name, added, items - added);
        } else {
            ESP_LOGI(TAG, "Pack overlay %s: %d items", l->name, added);
        }
        map_overlay_changed(l->overlay);
    }
}

bool maps_layers_open(map_view_t *view, map_pack_t *pack, lv_obj_t *root, int bottom)
{
    memset(&s, 0, sizeof s);
    s.view = view;
    s.pack = pack;
    s.root = root;
    s.bottom = bottom;
    init_styles();
    s.layers = alloc_psram(sizeof(layer_t) * MAX_LAYERS);
    s.points = alloc_psram(sizeof(map_lat_lon_t) * MAP_OVERLAY_MAX_VERTICES);
    if (!s.layers || !s.points) {
        ESP_LOGE(TAG, "no memory for the overlays");
        free(s.layers);
        free(s.points);
        s.layers = NULL;
        s.points = NULL;
        return false;
    }
    open_pack_overlays(view);
    find_files();
    int packs = s.count;
    for (int f = 0; f < s.files; f++) {
        layer_t *l = &s.layers[packs + f];
        size_t n = strlen(l->file) - (strstr(l->file, ".geojson") ? 8 : 5);
        snprintf(l->name, sizeof l->name, "%.*s", (int)n, l->file);
        l->overlay = map_overlay_create(view, l->name, f + 1, tap_cb, l);
        if (!l->overlay) {
            ESP_LOGE(TAG, "no memory for overlay %s", l->file);
            s.files = f;
            break;
        }
        s.count++;
        char path[sizeof OVERLAYS_DIR + FILE_NAME_MAX + 1];
        snprintf(path, sizeof path, "%s/%s", OVERLAYS_DIR, l->file);
        struct stat st;
        if (stat(path, &st) == 0) load(l, &st);
        map_overlay_changed(l->overlay);
    }
    map_view_set_miss_cb(view, miss_cb, NULL);
    if (s.count == 0) return false;  /* no buttons for nothing to switch */
    lv_obj_add_event_cb(map_view_obj(view), key_cb, LV_EVENT_KEY, NULL);
    s.layers_button = corner_button(LV_SYMBOL_LIST, LV_ALIGN_TOP_LEFT, MARGIN, layers_cb, "layers");
    if (s.files == 0) return false;  /* the Pack's do not change */
    corner_button(LV_SYMBOL_REFRESH, LV_ALIGN_TOP_RIGHT, -MARGIN, refresh_cb, "refresh");
    s.watch = lv_timer_create(watch_cb, WATCH_MS, NULL);
    return true;
}

void maps_layers_close(void)
{
    if (s.watch) lv_timer_delete(s.watch);
    for (int i = 0; i < s.count; i++) {
        map_overlay_destroy(s.layers[i].overlay);
        free(s.layers[i].text);
    }
    free(s.layers);
    free(s.points);
    /* The widgets go with the root. */
    memset(&s, 0, sizeof s);
}
