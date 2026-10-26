/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — App Store UI
 *
 * Two tabs:
 *   Catalog   — apps, drivers and window managers in the catalog
 *   Installed — the SD card's apps, as the boot scan loads them; those the
 *               catalog has a newer release of (update_manager.rs, the
 *               kernel's app offer) are marked, and their details have
 *               Update, which installs the release as the Catalog's Install
 *
 * The catalog's firmware is Settings → System update's (update_manager.rs),
 * which keeps the same copy of the catalog.
 *
 * The catalog shown is the one the server last sent, kept whole on the SD
 * card (APPSTORE_CATALOG_PATH) and read by the kernel's parser. Refresh fetches
 * it again and Install downloads an entry, both on the App Store's worker
 * task (appstore_job.c), never on lvgl_task.
 *
 * Each list is one lv_table, as the Phone app's and the To-do's are: a row
 * costs its text in LVGL's 64 KB pool. A row of widgets styled a property at
 * a time cost over 1 KB, and fourteen installed apps ran the pool dry. Every
 * widget here takes its look from the shared styles below.
 */
#include "appstore/appstore_app.h"
#include "appstore/appstore_job.h"

#include "ui/manager.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "thistle/appstore_client.h"
#include "thistle/net_manager.h"
#include "thistle/update.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_attr.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "appstore_ui";

/* ------------------------------------------------------------------ */
/* Layout constants                                                     */
/* ------------------------------------------------------------------ */

static int s_app_w = 240;
static int s_app_h = 296;
#define TITLE_BAR_H      30
#define STATUS_BAR_H     20
#define BTN_ROW_H        28
#define ITEM_PAD_LEFT     8
#define ITEM_PAD_RIGHT    6
#define TAB_W            74   /* fits "Installed" in Montserrat 14 and the padding */
#define TAB_GAP           2
#define ACTION_GAP        8
#define SCROLLBAR_ROOM    4
#define ELLIPSIS         "..."
#define CELL_MAX        200   /* a row's two lines */

#define MAX_CATALOG_APPS CATALOG_MAX_ENTRIES
#define INSTALLED_MAX    32
#define ROWS_MAX         32
_Static_assert(MAX_CATALOG_APPS <= ROWS_MAX && INSTALLED_MAX <= ROWS_MAX, "a page has a row per entry");

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    TAB_CATALOG,
    TAB_INSTALLED,
    TAB_COUNT,
} appstore_tab_t;

/* A tab's page: its list, what it says when the list is empty, a status line. */
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *table;
    lv_obj_t *empty;
    lv_obj_t *status;
    int rows[ROWS_MAX];   /* a row's index in entries[] or installed[] */
    int row_count;
} page_t;

/* The catalog is far too large for internal RAM: one catalog_entry_t carries
 * several URLs, three screenshot URLs and a changelog, and this holds
 * CATALOG_MAX_ENTRIES of them. It lives in PSRAM, which the board configures
 * for exactly this (CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY). */
static EXT_RAM_BSS_ATTR struct {
    lv_obj_t *root;
    lv_obj_t *tab_btns[TAB_COUNT];
    page_t pages[TAB_COUNT];
    lv_obj_t *detail_screen;

    catalog_entry_t entries[MAX_CATALOG_APPS];
    int entry_count;
    int selected_idx;        /* the entry the detail screen shows, or -1 */
    int selected_installed;  /* the installed app it shows, or -1 */

    installed_app_t installed[INSTALLED_MAX];
    int installed_count;

    appstore_tab_t current_tab;
    bool filling;            /* tables being filled: not a choice */
} s_store;

/* ------------------------------------------------------------------ */
/* Styles                                                               */
/* ------------------------------------------------------------------ */

static struct {
    lv_style_t panel;
    lv_style_t title_bar;
    lv_style_t tab;
    lv_style_t tab_active;
    lv_style_t button;
    lv_style_t button_pressed;
    lv_style_t table;
    lv_style_t cell;
    lv_style_t cell_chosen;
    lv_style_t scrollbar;
    lv_style_t status_bar;
    lv_style_t text;
    lv_style_t secondary;
    lv_style_t heading;
    lv_style_t accent;
    lv_style_t rule;
    lv_style_t content;
} s_styles;
static bool s_styles_made;

static void styles_reset(void)
{
    if (!s_styles_made) return;
    lv_style_t *all = (lv_style_t *)&s_styles;
    for (size_t i = 0; i < sizeof s_styles / sizeof(lv_style_t); i++) lv_style_reset(&all[i]);
    s_styles_made = false;
}

static void styles_init(void)
{
    styles_reset();
    lv_style_t *all = (lv_style_t *)&s_styles;
    for (size_t i = 0; i < sizeof s_styles / sizeof(lv_style_t); i++) lv_style_init(&all[i]);
    s_styles_made = true;

    const theme_colors_t *tc = theme_get_colors();

    lv_style_set_bg_color(&s_styles.panel, tc->bg);
    lv_style_set_bg_opa(&s_styles.panel, LV_OPA_COVER);
    lv_style_set_border_width(&s_styles.panel, 0);
    lv_style_set_radius(&s_styles.panel, 0);
    lv_style_set_pad_all(&s_styles.panel, 0);
    lv_style_set_text_color(&s_styles.panel, tc->text);
    lv_style_set_text_font(&s_styles.panel, &lv_font_montserrat_14);

    lv_style_set_bg_color(&s_styles.title_bar, tc->bg);
    lv_style_set_bg_opa(&s_styles.title_bar, LV_OPA_COVER);
    lv_style_set_border_side(&s_styles.title_bar, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&s_styles.title_bar, tc->text);
    lv_style_set_border_width(&s_styles.title_bar, 1);
    lv_style_set_radius(&s_styles.title_bar, 0);
    lv_style_set_pad_left(&s_styles.title_bar, ITEM_PAD_LEFT);
    lv_style_set_pad_right(&s_styles.title_bar, ITEM_PAD_RIGHT);
    lv_style_set_pad_ver(&s_styles.title_bar, 0);
    lv_style_set_text_color(&s_styles.title_bar, tc->text);
    lv_style_set_text_font(&s_styles.title_bar, &lv_font_montserrat_14);

    lv_style_set_bg_color(&s_styles.tab, tc->surface);
    lv_style_set_bg_opa(&s_styles.tab, LV_OPA_COVER);
    lv_style_set_border_color(&s_styles.tab, tc->text);
    lv_style_set_border_width(&s_styles.tab, 1);
    lv_style_set_radius(&s_styles.tab, 0);
    lv_style_set_pad_all(&s_styles.tab, 2);
    lv_style_set_text_color(&s_styles.tab, tc->text);
    lv_style_set_text_font(&s_styles.tab, &lv_font_montserrat_14);

    lv_style_set_bg_color(&s_styles.tab_active, tc->primary);
    lv_style_set_text_color(&s_styles.tab_active, lv_color_white());

    lv_style_set_bg_color(&s_styles.button, tc->surface);
    lv_style_set_bg_opa(&s_styles.button, LV_OPA_COVER);
    lv_style_set_border_color(&s_styles.button, tc->text);
    lv_style_set_border_width(&s_styles.button, 1);
    lv_style_set_radius(&s_styles.button, 0);
    lv_style_set_pad_all(&s_styles.button, 4);
    lv_style_set_text_color(&s_styles.button, tc->text);
    lv_style_set_text_font(&s_styles.button, &lv_font_montserrat_14);

    lv_style_set_bg_color(&s_styles.button_pressed, tc->primary);
    lv_style_set_text_color(&s_styles.button_pressed, lv_color_white());

    lv_style_set_bg_opa(&s_styles.table, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_styles.table, 0);
    lv_style_set_radius(&s_styles.table, 0);
    lv_style_set_pad_all(&s_styles.table, 0);

    lv_style_set_bg_color(&s_styles.cell, tc->bg);
    lv_style_set_bg_opa(&s_styles.cell, LV_OPA_COVER);
    lv_style_set_radius(&s_styles.cell, 0);
    lv_style_set_pad_left(&s_styles.cell, ITEM_PAD_LEFT);
    lv_style_set_pad_right(&s_styles.cell, ITEM_PAD_RIGHT);
    lv_style_set_pad_ver(&s_styles.cell, 3);
    lv_style_set_text_color(&s_styles.cell, tc->text);
    lv_style_set_text_font(&s_styles.cell, &lv_font_montserrat_14);

    /* The row under a finger, or chosen from the keys, inverted. */
    lv_style_set_bg_color(&s_styles.cell_chosen, tc->primary);
    lv_style_set_text_color(&s_styles.cell_chosen, tc->bg);

    lv_style_set_bg_color(&s_styles.scrollbar, tc->primary);
    lv_style_set_bg_opa(&s_styles.scrollbar, LV_OPA_COVER);
    lv_style_set_width(&s_styles.scrollbar, 2);
    lv_style_set_radius(&s_styles.scrollbar, 0);

    lv_style_set_bg_color(&s_styles.status_bar, tc->bg);
    lv_style_set_bg_opa(&s_styles.status_bar, LV_OPA_COVER);
    lv_style_set_border_side(&s_styles.status_bar, LV_BORDER_SIDE_TOP);
    lv_style_set_border_color(&s_styles.status_bar, tc->text);
    lv_style_set_border_width(&s_styles.status_bar, 1);
    lv_style_set_radius(&s_styles.status_bar, 0);
    lv_style_set_pad_left(&s_styles.status_bar, ITEM_PAD_LEFT);
    lv_style_set_pad_right(&s_styles.status_bar, ITEM_PAD_RIGHT);
    lv_style_set_pad_ver(&s_styles.status_bar, 0);

    lv_style_set_text_color(&s_styles.text, tc->text);
    lv_style_set_text_font(&s_styles.text, &lv_font_montserrat_14);

    lv_style_set_text_color(&s_styles.secondary, tc->text_secondary);
    lv_style_set_text_font(&s_styles.secondary, &lv_font_montserrat_14);

    lv_style_set_text_color(&s_styles.heading, tc->text);
    lv_style_set_text_font(&s_styles.heading, &lv_font_montserrat_18);

    lv_style_set_text_color(&s_styles.accent, tc->primary);
    lv_style_set_text_font(&s_styles.accent, &lv_font_montserrat_14);

    lv_style_set_bg_color(&s_styles.rule, tc->text);
    lv_style_set_bg_opa(&s_styles.rule, LV_OPA_50);
    lv_style_set_border_width(&s_styles.rule, 0);
    lv_style_set_radius(&s_styles.rule, 0);

    lv_style_set_bg_color(&s_styles.content, tc->bg);
    lv_style_set_bg_opa(&s_styles.content, LV_OPA_COVER);
    lv_style_set_border_width(&s_styles.content, 0);
    lv_style_set_radius(&s_styles.content, 0);
    lv_style_set_pad_left(&s_styles.content, ITEM_PAD_LEFT);
    lv_style_set_pad_right(&s_styles.content, ITEM_PAD_RIGHT);
    lv_style_set_pad_ver(&s_styles.content, 6);
    lv_style_set_pad_row(&s_styles.content, 4);
}

/* ------------------------------------------------------------------ */
/* Widget helpers                                                       */
/* ------------------------------------------------------------------ */

static lv_obj_t *make_box(lv_obj_t *parent, const lv_style_t *style)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, style, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_style_t *style, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_add_style(label, style, LV_PART_MAIN);
    lv_label_set_text(label, text);
    return label;
}

/* A label with no style of its own, the theme's included, so that it takes
 * its colour and font from its parent in whatever state that is in: the
 * theme's text colour kept a chosen tab's label black on black. */
static lv_obj_t *make_inheriting_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, const lv_style_t *style, const lv_style_t *pressed,
                             const char *text, int32_t width, int32_t height,
                             lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, style, LV_PART_MAIN);
    /* Chosen from the keys, it shows as pressed: the theme's focus ring went
     * with its styles. */
    lv_obj_add_style(btn, pressed, LV_STATE_PRESSED);
    lv_obj_add_style(btn, pressed, LV_STATE_FOCUS_KEY);
    lv_obj_set_size(btn, width, height);
    lv_obj_t *label = make_inheriting_label(btn, text);
    lv_obj_center(label);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

/* text, cut with an ellipsis to fit width, on one line, into out. */
static void fit(const char *text, const lv_font_t *font, int32_t width, char *out, size_t cap)
{
    if (!cap) return;
    snprintf(out, cap, "%s", text ? text : "");
    for (char *c = out; *c; c++) {
        if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
    }
    size_t len = strlen(out);
    if (lv_text_get_width(out, (uint32_t)len, font, 0) <= width) return;

    int32_t room = width - lv_text_get_width(ELLIPSIS, sizeof(ELLIPSIS) - 1, font, 0);
    size_t end = len;
    while (end > 0 && lv_text_get_width(out, (uint32_t)end, font, 0) > room) {
        do end--; while (end > 0 && ((unsigned char)out[end] & 0xC0) == 0x80);
    }
    while (end > 0 && out[end - 1] == ' ') end--;
    if (end + sizeof(ELLIPSIS) > cap) end = cap - sizeof(ELLIPSIS);
    memcpy(out + end, ELLIPSIS, sizeof(ELLIPSIS));
}

/* A row's two lines, each cut to the table's width. */
static void row_text(const char *first, const char *second, char *out, size_t cap)
{
    const lv_font_t *font = &lv_font_montserrat_14;
    int32_t width = s_app_w - ITEM_PAD_LEFT - ITEM_PAD_RIGHT - SCROLLBAR_ROOM;
    char line1[CELL_MAX / 2], line2[CELL_MAX / 2];
    fit(first, font, width, line1, sizeof line1);
    fit(second, font, width, line2, sizeof line2);
    snprintf(out, cap, "%s\n%s", line1, line2);
}

static void set_status(appstore_tab_t tab, const char *text)
{
    lv_obj_t *label = s_store.pages[tab].status;
    if (label && strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

/* ------------------------------------------------------------------ */
/* Data                                                                 */
/* ------------------------------------------------------------------ */

static esp_err_t load_catalog_local(void)
{
    int count = 0;
    esp_err_t err = appstore_load_catalog_file(APPSTORE_CATALOG_PATH, s_store.entries,
                                               MAX_CATALOG_APPS, &count);
    s_store.entry_count = err == ESP_OK ? count : 0;
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "No catalog cache at %s", APPSTORE_CATALOG_PATH);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot read %s: %s", APPSTORE_CATALOG_PATH, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Loaded %d entries from local cache", count);
    }
    return err;
}

static void scan_installed_apps(void)
{
    int count = 0;
    if (appstore_list_installed(s_store.installed, INSTALLED_MAX, &count) != ESP_OK) count = 0;
    s_store.installed_count = count;
    ESP_LOGI(TAG, "Found %d installed apps", count);
}

/* The installed app with this ID, or -1. */
static int installed_index(const char *id)
{
    for (int i = 0; i < s_store.installed_count; i++) {
        if (strcmp(s_store.installed[i].id, id) == 0) return i;
    }
    return -1;
}

/* The catalog's release of `id` that the kernel offers as an update to it,
 * or -1: the kernel decides (a newer release, a min_os this firmware
 * meets), the entry is the catalog's as this screen read it. */
static int update_index(const char *id)
{
    char version[sizeof(s_store.entries[0].version)];
    if (!rs_update_app_offer(id, version, sizeof(version))) return -1;
    for (int i = 0; i < s_store.entry_count; i++) {
        const catalog_entry_t *e = &s_store.entries[i];
        if (e->type == CATALOG_TYPE_APP && strcmp(e->id, id) == 0 && strcmp(e->version, version) == 0) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Lists                                                                */
/* ------------------------------------------------------------------ */

static void page_finish(page_t *page, const char *empty_text)
{
    lv_table_set_row_count(page->table, page->row_count ? (uint32_t)page->row_count : 1);
    if (!page->row_count) lv_table_set_cell_value(page->table, 0, 0, "");
    s_store.filling = false;

    bool empty = page->row_count == 0;
    if (empty) lv_label_set_text(page->empty, empty_text);
    lv_obj_update_flag(page->table, LV_OBJ_FLAG_HIDDEN, empty);
    lv_obj_update_flag(page->empty, LV_OBJ_FLAG_HIDDEN, !empty);
}

static void page_add_row(page_t *page, int index, const char *text)
{
    if (page->row_count >= ROWS_MAX) return;
    lv_table_set_cell_value(page->table, (uint32_t)page->row_count, 0, text);
    page->rows[page->row_count++] = index;
}

/* Catalog: apps, drivers and window managers. */
static void build_catalog_list(void)
{
    page_t *page = &s_store.pages[TAB_CATALOG];
    if (!page->table) return;

    s_store.filling = true;
    page->row_count = 0;
    char head[128], cell[CELL_MAX];
    for (int i = 0; i < s_store.entry_count; i++) {
        const catalog_entry_t *e = &s_store.entries[i];
        if (e->type == CATALOG_TYPE_FIRMWARE) continue;
        snprintf(head, sizeof head, "%s%s%s  %s", e->name,
                 installed_index(e->id) >= 0 ? " [i]" : "",
                 e->type == CATALOG_TYPE_DRIVER ? " [drv]" : e->type == CATALOG_TYPE_WM ? " [wm]" : "",
                 e->version);
        row_text(head, e->description, cell, sizeof cell);
        page_add_row(page, i, cell);
    }
    page_finish(page, "No apps in the catalog.\nRefresh from the server, or place catalog.json in "
                      "/sdcard/appstore.");

    char status[48];
    snprintf(status, sizeof status, "Catalog: %d app(s)", page->row_count);
    set_status(TAB_CATALOG, status);
}

/* Installed: the SD card's apps. */
static void build_installed_list(void)
{
    page_t *page = &s_store.pages[TAB_INSTALLED];
    if (!page->table) return;

    s_store.filling = true;
    page->row_count = 0;
    int updates = 0;
    char head[96], detail[64], cell[CELL_MAX];
    for (int i = 0; i < s_store.installed_count; i++) {
        const installed_app_t *app = &s_store.installed[i];
        int update = update_index(app->id);
        snprintf(head, sizeof head, "%s  %s", app->name, app->version);
        if (update >= 0) {
            updates++;
            snprintf(detail, sizeof detail, "Update: %s", s_store.entries[update].version);
            ESP_LOGI(TAG, "Update offered: %s %s -> %s", app->id, app->version,
                     s_store.entries[update].version);
        } else {
            snprintf(detail, sizeof detail, "%u KB, %s", (unsigned)((app->size_bytes + 1023) / 1024),
                     app->is_package ? "from the App Store" : "an ELF on the card");
        }
        row_text(head, detail, cell, sizeof cell);
        page_add_row(page, i, cell);
    }
    page_finish(page, "No apps installed.\nBrowse the Catalog tab.");

    char status[48];
    if (updates > 0) {
        snprintf(status, sizeof status, "%d app(s) installed, %d update(s)", page->row_count, updates);
    } else {
        snprintf(status, sizeof status, "%d app(s) installed", page->row_count);
    }
    set_status(TAB_INSTALLED, status);
}

/* ------------------------------------------------------------------ */
/* Tabs                                                                 */
/* ------------------------------------------------------------------ */

static void show_tab(appstore_tab_t tab)
{
    s_store.current_tab = tab;
    for (int i = 0; i < TAB_COUNT; i++) {
        if (s_store.tab_btns[i]) lv_obj_set_state(s_store.tab_btns[i], LV_STATE_CHECKED, i == (int)tab);
        if (s_store.pages[i].screen) {
            lv_obj_update_flag(s_store.pages[i].screen, LV_OBJ_FLAG_HIDDEN,
                               i != (int)tab || s_store.detail_screen);
        }
    }
    if (tab == TAB_INSTALLED) {
        scan_installed_apps();
        build_installed_list();
    }
}

static void tab_cb(lv_event_t *e)
{
    appstore_tab_t tab = (appstore_tab_t)(intptr_t)lv_event_get_user_data(e);
    if (tab != s_store.current_tab) show_tab(tab);
}

/* ------------------------------------------------------------------ */
/* Detail screens                                                       */
/* ------------------------------------------------------------------ */

static void close_detail(void)
{
    if (s_store.detail_screen) {
        lv_obj_delete(s_store.detail_screen);
        s_store.detail_screen = NULL;
    }
    s_store.selected_idx = -1;
    s_store.selected_installed = -1;
    show_tab(s_store.current_tab);
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    close_detail();
}

/* The Back key: from a detail screen to the tab it was opened from. */
static bool appstore_back(void)
{
    if (!s_store.detail_screen) return false;
    close_detail();
    return true;
}

/* A detail screen over the tabs, titled; returns its scrolling column. */
static lv_obj_t *open_detail(const char *title)
{
    if (s_store.detail_screen) lv_obj_delete(s_store.detail_screen);
    for (int i = 0; i < TAB_COUNT; i++) lv_obj_add_flag(s_store.pages[i].screen, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *screen = make_box(s_store.root, &s_styles.panel);
    lv_obj_set_pos(screen, 0, 0);
    lv_obj_set_size(screen, s_app_w, s_app_h);
    s_store.detail_screen = screen;

    lv_obj_t *bar = make_box(screen, &s_styles.title_bar);
    lv_obj_add_style(bar, &s_styles.button_pressed, LV_STATE_PRESSED);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, s_app_w, TITLE_BAR_H);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bar, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back = make_inheriting_label(bar, "< Back");
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_remove_style_all(content);
    lv_obj_add_style(content, &s_styles.content, LV_PART_MAIN);
    lv_obj_add_style(content, &s_styles.scrollbar, LV_PART_SCROLLBAR);
    lv_obj_set_pos(content, 0, TITLE_BAR_H);
    lv_obj_set_size(content, s_app_w, s_app_h - TITLE_BAR_H);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);

    lv_obj_t *name = make_label(content, &s_styles.heading, title);
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_t *rule = make_box(content, &s_styles.rule);
    lv_obj_set_size(rule, LV_PCT(100), 1);
    return content;
}

static lv_obj_t *detail_line(lv_obj_t *content, const lv_style_t *style, const char *fmt, ...)
{
    char text[288];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    lv_obj_t *label = make_label(content, style, text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static void detail_rule(lv_obj_t *content)
{
    lv_obj_t *rule = make_box(content, &s_styles.rule);
    lv_obj_set_size(rule, LV_PCT(100), 1);
}

static lv_obj_t *detail_buttons(lv_obj_t *content)
{
    lv_obj_t *row = make_box(content, &s_styles.panel);
    lv_obj_set_size(row, LV_PCT(100), 34);
    return row;
}

static void size_text(uint32_t bytes, char *out, size_t cap)
{
    if (bytes) snprintf(out, cap, "%u KB", (unsigned)((bytes + 1023) / 1024));
    else snprintf(out, cap, "unknown");
}

/* ------------------------------------------------------------------ */
/* Install, refresh and remove                                          */
/* ------------------------------------------------------------------ */

/* A catalog, or a package it lists, copied onto the card needs no network
 * (appstore_client.rs, card_file()). */
static bool on_card(const char *url)
{
    return strncmp(url, "file://", 7) == 0;
}

static bool start_job(appstore_job_kind_t kind, const catalog_entry_t *entry, const char *status)
{
    const char *source = !entry                 ? appstore_get_catalog_url()
                         : entry->package_url[0] ? entry->package_url
                                                 : entry->url;
    if (!on_card(source) && !net_is_connected()) {
        toast_warn("No network connection");
        return false;
    }
    if (!appstore_job_start(kind, entry)) return false;
    set_status(TAB_CATALOG, status);
    return true;
}

static void install_btn_cb(lv_event_t *e)
{
    (void)e;
    int idx = s_store.selected_idx;
    if (idx < 0 || idx >= s_store.entry_count) return;
    const catalog_entry_t *entry = &s_store.entries[idx];

    char status[80];
    snprintf(status, sizeof status, "Installing %s...", entry->name);
    if (start_job(APPSTORE_JOB_INSTALL, entry, status)) {
        toast_show("Downloading...", TOAST_INFO, 3000);
        close_detail();
    }
}

/* Update: the release the kernel offers, installed through the same TAP
 * path as an install from the Catalog tab. */
static void update_btn_cb(lv_event_t *e)
{
    (void)e;
    int i = s_store.selected_installed;
    if (i < 0 || i >= s_store.installed_count) return;
    int idx = update_index(s_store.installed[i].id);
    if (idx < 0) return;
    const catalog_entry_t *entry = &s_store.entries[idx];

    char status[80];
    snprintf(status, sizeof status, "Updating %s to %s...", entry->name, entry->version);
    ESP_LOGI(TAG, "Updating %s to %s", entry->id, entry->version);
    if (start_job(APPSTORE_JOB_INSTALL, entry, status)) {
        set_status(TAB_INSTALLED, status);
        toast_show("Downloading...", TOAST_INFO, 3000);
        close_detail();
    }
}

static void refresh_server_cb(lv_event_t *e)
{
    (void)e;
    if (start_job(APPSTORE_JOB_REFRESH, NULL, "Fetching catalog...")) {
        toast_show("Fetching the catalog...", TOAST_INFO, 3000);
    }
}

static void refresh_local_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = load_catalog_local();
    build_catalog_list();

    if (err == ESP_ERR_NOT_FOUND) {
        set_status(TAB_CATALOG, "catalog.json not found on SD");
        toast_warn("catalog.json missing from SD card");
    } else if (err != ESP_OK) {
        set_status(TAB_CATALOG, "Catalog load error");
        toast_warn("Failed to load catalog");
    }
}

static void remove_installed_app(int i)
{
    if (i < 0 || i >= s_store.installed_count) return;
    const installed_app_t *app = &s_store.installed[i];
    char msg[128];
    if (appstore_remove_installed(app->key) == ESP_OK) {
        ESP_LOGI(TAG, "Removed: %s", app->key);
        snprintf(msg, sizeof msg, "Removed %s. It leaves the launcher at the next restart.", app->name);
        toast_show(msg, TOAST_SUCCESS, 3000);
    } else {
        ESP_LOGW(TAG, "Failed to remove: %s", app->key);
        snprintf(msg, sizeof msg, "Could not remove %s", app->name);
        toast_warn(msg);
    }
    scan_installed_apps();
    build_catalog_list();
    close_detail();
}

static void remove_entry_cb(lv_event_t *e)
{
    (void)e;
    int idx = s_store.selected_idx;
    if (idx < 0 || idx >= s_store.entry_count) return;
    remove_installed_app(installed_index(s_store.entries[idx].id));
}

static void remove_selected_installed_cb(lv_event_t *e)
{
    (void)e;
    remove_installed_app(s_store.selected_installed);
}

/* A job has ended (appstore_job.c has said how, in a toast). */
static void job_done_cb(const appstore_job_result_t *result)
{
    if (!s_store.root) return;

    if (result->kind == APPSTORE_JOB_REFRESH) {
        if (result->err == ESP_OK && result->count > 0) {
            /* The details shown may be of an entry about to move. */
            if (s_store.selected_idx >= 0) close_detail();
            int count = result->count < MAX_CATALOG_APPS ? result->count : MAX_CATALOG_APPS;
            memcpy(s_store.entries, result->entries, sizeof(catalog_entry_t) * (size_t)count);
            s_store.entry_count = count;
        }
        build_catalog_list();

        char status[64];
        if (result->err != ESP_OK) {
            ESP_LOGW(TAG, "Server fetch failed (%s), keeping the cached catalog",
                     esp_err_to_name(result->err));
            snprintf(status, sizeof status, "%s",
                     s_store.entry_count ? "Server failed - showing cache" : "Server failed - no cache");
        } else if (result->count == 0) {
            snprintf(status, sizeof status, "Nothing for this device");
        } else {
            snprintf(status, sizeof status, "Fetched %d entries from server", result->count);
        }
        set_status(TAB_CATALOG, status);
        return;
    }

    scan_installed_apps();
    build_catalog_list();
    build_installed_list();
}

/* ------------------------------------------------------------------ */
/* Detail screens' content                                              */
/* ------------------------------------------------------------------ */

static void show_detail(int idx)
{
    if (idx < 0 || idx >= s_store.entry_count) return;
    const catalog_entry_t *entry = &s_store.entries[idx];

    lv_obj_t *content = open_detail(entry->name);
    s_store.selected_idx = idx;

    const char *type =
        entry->type == CATALOG_TYPE_DRIVER   ? "Driver" :
        entry->type == CATALOG_TYPE_WM       ? "Window Manager" : "App";
    char size[24];
    size_text(entry->package_size_bytes ? entry->package_size_bytes : entry->size_bytes,
              size, sizeof size);

    detail_line(content, &s_styles.accent, "Type: %s", type);
    detail_line(content, &s_styles.text, "Version: %s", entry->version);
    detail_line(content, &s_styles.text, "Author: %s", entry->author);
    detail_line(content, &s_styles.text, "Size: %s", size);
    detail_line(content, &s_styles.text, "Permissions: %s",
                entry->permissions[0] ? entry->permissions : "none");
    detail_line(content, &s_styles.text, "Signed: %s", entry->is_signed ? "Yes" : "No");
    detail_rule(content);
    detail_line(content, &s_styles.secondary, "%s", entry->description);
    detail_rule(content);

    lv_obj_t *row = detail_buttons(content);
    lv_obj_t *btn = make_button(row, &s_styles.button, &s_styles.button_pressed, "Install", 100, 26,
                                install_btn_cb, NULL);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 0, 0);
    if (entry->type == CATALOG_TYPE_APP && installed_index(entry->id) >= 0) {
        btn = make_button(row, &s_styles.button, &s_styles.button_pressed, "Remove", 100, 26,
                          remove_entry_cb, NULL);
        lv_obj_align(btn, LV_ALIGN_LEFT_MID, 110, 0);
    }
}

static void show_installed_detail(int i)
{
    if (i < 0 || i >= s_store.installed_count) return;
    const installed_app_t *app = &s_store.installed[i];

    lv_obj_t *content = open_detail(app->name);
    s_store.selected_installed = i;

    char size[24];
    size_text(app->size_bytes, size, sizeof size);
    detail_line(content, &s_styles.accent, "%s",
                app->is_package ? "Installed from the App Store" : "An ELF copied onto the card");
    detail_line(content, &s_styles.text, "Version: %s", app->version[0] ? app->version : "unknown");
    detail_line(content, &s_styles.text, "ID: %s", app->id);
    detail_line(content, &s_styles.text, "Size: %s", size);
    int update = update_index(app->id);
    if (update >= 0) {
        detail_line(content, &s_styles.accent, "Update: %s", s_store.entries[update].version);
    }
    detail_rule(content);

    lv_obj_t *row = detail_buttons(content);
    int32_t x = 0;
    lv_obj_t *btn;
    if (update >= 0) {
        btn = make_button(row, &s_styles.button, &s_styles.button_pressed, "Update", 100, 26,
                          update_btn_cb, NULL);
        lv_obj_align(btn, LV_ALIGN_LEFT_MID, 0, 0);
        x = 110;
    }
    btn = make_button(row, &s_styles.button, &s_styles.button_pressed, "Remove", 100, 26,
                      remove_selected_installed_cb, NULL);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, x, 0);
}

static void row_chosen_cb(lv_event_t *e)
{
    if (s_store.filling) return;
    /* The selection also moves with the arrow keys, which must not open. */
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD && lv_indev_get_key(indev) != LV_KEY_ENTER) {
        return;
    }
    lv_obj_t *table = lv_event_get_target(e);
    uint32_t row = 0, col = 0;
    lv_table_get_selected_cell(table, &row, &col);
    for (int tab = 0; tab < TAB_COUNT; tab++) {
        page_t *page = &s_store.pages[tab];
        if (page->table != table) continue;
        if (row == LV_TABLE_CELL_NONE || row >= (uint32_t)page->row_count) return;
        if (tab == TAB_INSTALLED) show_installed_detail(page->rows[row]);
        else show_detail(page->rows[row]);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Pages                                                                */
/* ------------------------------------------------------------------ */

static lv_obj_t *make_table(lv_obj_t *parent, int32_t height)
{
    lv_obj_t *table = lv_table_create(parent);
    lv_obj_remove_style_all(table);
    lv_obj_add_style(table, &s_styles.table, LV_PART_MAIN);
    lv_obj_add_style(table, &s_styles.cell, LV_PART_ITEMS);
    lv_obj_add_style(table, &s_styles.cell_chosen, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_style(table, &s_styles.cell_chosen, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(table, &s_styles.scrollbar, LV_PART_SCROLLBAR);
    theme_rule_table_rows(table);
    lv_obj_set_pos(table, 0, 0);
    lv_obj_set_size(table, s_app_w, height > 1 ? height : 1);
    lv_obj_set_scrollbar_mode(table, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(table, LV_DIR_VER);
    lv_obj_remove_flag(table, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_table_set_column_count(table, 1);
    lv_table_set_column_width(table, 0, s_app_w);
    lv_obj_add_event_cb(table, row_chosen_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return table;
}

/* A tab's page: its list, then a row of up to two buttons, then a status line. */
static void make_page(appstore_tab_t tab, const char *status,
                      const char *left, lv_event_cb_t left_cb,
                      const char *right, lv_event_cb_t right_cb)
{
    page_t *page = &s_store.pages[tab];
    int btn_row_h = left ? BTN_ROW_H : 0;
    int list_h = s_app_h - TITLE_BAR_H - STATUS_BAR_H - btn_row_h;

    page->screen = make_box(s_store.root, &s_styles.panel);
    lv_obj_set_pos(page->screen, 0, TITLE_BAR_H);
    lv_obj_set_size(page->screen, s_app_w, s_app_h - TITLE_BAR_H);
    lv_obj_add_flag(page->screen, LV_OBJ_FLAG_HIDDEN);

    page->table = make_table(page->screen, list_h);
    page->empty = make_label(page->screen, &s_styles.text, "");
    lv_obj_set_pos(page->empty, ITEM_PAD_LEFT, 8);
    lv_obj_set_width(page->empty, s_app_w - 2 * ITEM_PAD_LEFT);
    lv_label_set_long_mode(page->empty, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(page->empty, LV_OBJ_FLAG_HIDDEN);

    if (left) {
        lv_obj_t *row = make_box(page->screen, &s_styles.panel);
        lv_obj_set_pos(row, 0, list_h);
        lv_obj_set_size(row, s_app_w, BTN_ROW_H);
        /* Two buttons share the row; at 120 and 160 px they ran 56 px past a
         * 240 px screen. One alone is as wide as "Refresh from Server". */
        int action_w = right ? (s_app_w - ITEM_PAD_LEFT - ITEM_PAD_RIGHT - ACTION_GAP) / 2 : 160;
        lv_obj_t *btn = make_button(row, &s_styles.button, &s_styles.button_pressed, left,
                                    action_w, 26, left_cb, NULL);
        lv_obj_align(btn, LV_ALIGN_LEFT_MID, ITEM_PAD_LEFT, 0);
        if (right) {
            btn = make_button(row, &s_styles.button, &s_styles.button_pressed, right,
                              action_w, 26, right_cb, NULL);
            lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -ITEM_PAD_RIGHT, 0);
        }
    }

    lv_obj_t *bar = make_box(page->screen, &s_styles.status_bar);
    lv_obj_set_pos(bar, 0, list_h + btn_row_h);
    lv_obj_set_size(bar, s_app_w, STATUS_BAR_H);
    page->status = make_label(bar, &s_styles.text, status);
    lv_obj_align(page->status, LV_ALIGN_LEFT_MID, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Public API — build UI once                                           */
/* ------------------------------------------------------------------ */

esp_err_t appstore_ui_create(lv_obj_t *parent)
{
    if (s_store.root) {
        ESP_LOGW(TAG, "UI already created");
        return ESP_OK;
    }

    memset(&s_store, 0, sizeof(s_store));
    s_store.selected_idx = -1;
    s_store.selected_installed = -1;
    s_store.current_tab  = TAB_CATALOG;

    lv_obj_update_layout(parent);
    s_app_w = lv_obj_get_width(parent);
    s_app_h = lv_obj_get_height(parent);
    if (s_app_w == 0) s_app_w = 240;
    if (s_app_h == 0) s_app_h = 296;

    styles_init();

    s_store.root = make_box(parent, &s_styles.panel);
    lv_obj_set_size(s_store.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_store.root, 0, 0);

    /* ---------------------------------------------------------------
     * Shared title bar with two tab buttons
     * --------------------------------------------------------------- */
    lv_obj_t *title_bar = make_box(s_store.root, &s_styles.title_bar);
    lv_obj_set_pos(title_bar, 0, 0);
    lv_obj_set_size(title_bar, s_app_w, TITLE_BAR_H);

    /* The tab buttons on the right, as wide as the bar allows. Three 80 px
     * tabs were wider than a 240 px bar and pushed "Catalog" off its left
     * edge. The title goes in whatever the tabs leave, and only if all of it
     * fits: where it did not, it was drawn half under the first tab. */
    int bar_w = s_app_w - ITEM_PAD_LEFT - ITEM_PAD_RIGHT;
    int tab_w = LV_MIN(TAB_W, (bar_w - (TAB_COUNT - 1) * TAB_GAP) / TAB_COUNT);

    const char *title = "App Store";
    lv_obj_t *title_lbl = make_inheriting_label(title_bar, title);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    int title_room = bar_w - TAB_COUNT * tab_w - TAB_COUNT * TAB_GAP;
    if (lv_text_get_width(title, strlen(title), &lv_font_montserrat_14, 0) > title_room) {
        lv_obj_add_flag(title_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    static const char *const tab_names[TAB_COUNT] = { "Catalog", "Installed" };
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *btn = make_button(title_bar, &s_styles.tab, &s_styles.button_pressed, tab_names[i],
                                    tab_w, 22, tab_cb, (void *)(intptr_t)i);
        lv_obj_add_style(btn, &s_styles.tab_active, LV_STATE_CHECKED);
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -(TAB_COUNT - 1 - i) * (tab_w + TAB_GAP), 0);
        s_store.tab_btns[i] = btn;
    }

    make_page(TAB_CATALOG, "Loading...", "Reload Cache", refresh_local_cb, "Refresh", refresh_server_cb);
    make_page(TAB_INSTALLED, "Installed apps", NULL, NULL, NULL, NULL);

    /* ---------------------------------------------------------------
     * Initial data load from local cache
     * --------------------------------------------------------------- */
    esp_err_t err = load_catalog_local();
    scan_installed_apps();
    build_catalog_list();
    show_tab(TAB_CATALOG);

    if (appstore_job_busy()) {
        set_status(TAB_CATALOG, "Still working on the last request...");
    } else if (err == ESP_ERR_NOT_FOUND) {
        set_status(TAB_CATALOG, "catalog.json not found on SD");
    } else if (err != ESP_OK) {
        set_status(TAB_CATALOG, "Catalog load error");
    }
    appstore_job_set_done_cb(job_done_cb);

    ui_manager_set_back_handler(s_store.root, appstore_back);

    ESP_LOGI(TAG, "App Store UI created (%d entries loaded)", s_store.entry_count);
    return ESP_OK;
}

void appstore_ui_show(void)
{
    if (s_store.root) {
        lv_obj_remove_flag(s_store.root, LV_OBJ_FLAG_HIDDEN);
    }
}

void appstore_ui_hide(void)
{
    if (s_store.root) {
        lv_obj_add_flag(s_store.root, LV_OBJ_FLAG_HIDDEN);
    }
}

void appstore_ui_destroy(void)
{
    /* A job running carries on, and says how it went in a toast. */
    appstore_job_set_done_cb(NULL);
    if (s_store.root) {
        lv_obj_delete(s_store.root);
        s_store.root = NULL;
    }
    memset(s_store.pages, 0, sizeof(s_store.pages));
    s_store.detail_screen = NULL;
    styles_reset();
}
