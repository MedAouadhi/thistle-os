#include "launcher/launcher_app.h"
#include "launcher/home_core.h"

#include "lvgl.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "hal/board.h"
#include "thistle/app_manager.h"
#include "thistle/call.h"
#include "thistle/mesh.h"
#include "thistle/phonebook.h"
#include "thistle/sms.h"
#include "thistle/tailnet.h"
#include "thistle/time_manager.h"
#include "thistle/wifi_manager.h"
#include "ui/manager.h"
#include "ui/statusbar.h"
#include "ui/toast.h"
#include "ui/theme.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "launcher_ui";

/* The Home clock: the digits of time_manager_format_time() at 44 px, one bit
 * a pixel for e-paper (tools/gen_clock_font.sh). */
LV_FONT_DECLARE(home_clock_44);
LV_FONT_DECLARE(home_clock_44_mono);

/* What Montserrat 14 lacks of Latin-1 and Latin Extended-A, in the same face,
 * for a missed caller's name from the phonebook (tools/gen_latin_font.sh).
 * The detail line of what is new is Montserrat 14 with this as its
 * fallback. */
LV_FONT_DECLARE(home_latin_14);
LV_FONT_DECLARE(home_latin_14_mono);
static lv_font_t s_detail_font;

/* ------------------------------------------------------------------ */
/* Layout constants                                                     */
/* ------------------------------------------------------------------ */

/* App-area dimensions — set from parent in launcher_ui_create() */
static int s_app_w = 240;
static int s_app_h = 296;
#define CELL_SIZE        70
#define ICON_BOX         36   /* a pinned app's icon */
#define NAME_PAD          3   /* either side of its name */
#define FOCUS_ROOM        4   /* the colour focus ring, outside the icon */

/* How often Home looks at the clock. Its labels are rewritten only when the
 * minute or the day changes, so on e-paper a look that finds nothing new
 * costs no refresh. */
#define CLOCK_POLL_MS    1000

/* How often Home reads what is new. A row is rewritten only when what it
 * says changes. */
#define NOW_POLL_MS      1000
#define NOW_GAP             4   /* between two rows on a colour screen */
#define NOW_RULE            1   /* above, between and below the rows on e-paper */

/* ------------------------------------------------------------------ */
/* Pinned apps                                                          */
/* ------------------------------------------------------------------ */

/* The apps Home pins, first to last, as many as the screen has room for. One
 * that is not installed gives its place to the next installed app that is
 * not a tool. */
static const char *const s_pin_preference[] = {
    "com.thistle.phone",
    "com.thistle.messenger",
    "com.thistle.hermes",
    "com.thistle.podcasts",
    "com.thistle.notes",
    "com.thistle.reader",
};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_root        = NULL;
static lv_obj_t *s_app_drawer  = NULL;
static lv_obj_t *s_drawer_grid = NULL;
static lv_obj_t *s_folder      = NULL;
static lv_obj_t *s_folder_grid = NULL;
static bool      s_drawer_visible = false;
static bool      s_folder_visible = false;

/* Home */
static lv_obj_t   *s_home        = NULL;   /* its widgets, while it is in front */
static home_layout_t s_layout;
static bool        s_mono        = false;  /* e-paper: outlines and inversion */
static lv_obj_t   *s_clock_label = NULL;   /* NULL on a screen with no room */
static lv_obj_t   *s_date_label  = NULL;
static lv_timer_t *s_clock_timer = NULL;
static lv_obj_t   *s_apps_btn    = NULL;   /* All apps */

/* What is new: HOME_NOW_ROWS rows, built with Home and filled from
 * home_now_rows(). A row is a button holding its icon, title and detail,
 * and beside it, for a missed call, a button that calls back. */
typedef struct {
    lv_obj_t *row;
    lv_obj_t *main;
    lv_obj_t *icon;
    lv_obj_t *title;
    lv_obj_t *detail;
    lv_obj_t *back;
} now_slot_t;

static lv_obj_t      *s_now_band = NULL;   /* NULL on a screen with no room */
static now_slot_t     s_now[HOME_NOW_ROWS];
/* What the rows show. Like s_missed, read by the LVGL task alone, so in
 * PSRAM (CLAUDE.md, "Internal RAM runs out first"). */
static EXT_RAM_BSS_ATTR home_now_row_t s_now_rows[HOME_NOW_ROWS];
static int            s_now_count = -1;    /* -1: nothing shown yet */
static lv_timer_t    *s_now_timer = NULL;

/* The newest missed call, read from the log when the log changes, and its
 * name from the phonebook when either changes. */
static EXT_RAM_BSS_ATTR struct {
    bool             read;
    uint32_t         log_seq;
    bool             found;
    call_log_entry_t entry;
    bool             named;
    uint32_t         book_seq;
    char             name[PB_NAME_MAX];
} s_missed;

/* ------------------------------------------------------------------ */
/* Stable semantic icon mapping.  LVGL's built-in symbols keep the
 * launcher legible without per-WM bitmap assets.                      */
/* ------------------------------------------------------------------ */

static const char *app_icon_symbol(const char *app_id)
{
    if (strstr(app_id, "settings"))    return LV_SYMBOL_SETTINGS;
    if (strstr(app_id, "filemgr"))     return LV_SYMBOL_DIRECTORY;
    if (strstr(app_id, "filexfer"))    return LV_SYMBOL_UPLOAD;
    if (strstr(app_id, "tailscale"))   return LV_SYMBOL_SHUFFLE;
    if (strstr(app_id, "hermes"))      return LV_SYMBOL_CHARGE;
    if (strstr(app_id, "reader"))      return LV_SYMBOL_FILE;
    if (strstr(app_id, "messenger"))   return LV_SYMBOL_ENVELOPE;
    if (strstr(app_id, "phone"))       return LV_SYMBOL_CALL;
    if (strstr(app_id, "navigator"))   return LV_SYMBOL_GPS;
    if (strstr(app_id, "notes"))       return LV_SYMBOL_EDIT;
    if (strstr(app_id, "assistant"))   return LV_SYMBOL_EYE_OPEN;
    if (strstr(app_id, "appstore"))    return LV_SYMBOL_DOWNLOAD;
    if (strstr(app_id, "wifiscanner")) return LV_SYMBOL_WIFI;
    if (strstr(app_id, "flashlight"))  return LV_SYMBOL_POWER;
    if (strstr(app_id, "weather"))     return LV_SYMBOL_REFRESH;
    if (strstr(app_id, "terminal"))    return LV_SYMBOL_KEYBOARD;
    if (strstr(app_id, "vault"))       return LV_SYMBOL_WARNING;
    if (strstr(app_id, "podcasts"))    return LV_SYMBOL_AUDIO;
    if (strstr(app_id, "radio"))       return LV_SYMBOL_AUDIO;
    return LV_SYMBOL_FILE;
}

static bool is_launcher(const char *app_id)
{
    return strcmp(app_id, "com.thistle.launcher") == 0 ||
           strcmp(app_id, "com.thistle.tk_launcher") == 0 ||
           strcmp(app_id, "com.thistle.tk-launcher") == 0;
}

static bool is_tool_app(const char *app_id)
{
    return strstr(app_id, "settings") || strstr(app_id, "terminal") ||
           strstr(app_id, "wifiscanner") || strstr(app_id, "flashlight");
}

static bool pin_eligible(const char *app_id)
{
    return !is_launcher(app_id) && !is_tool_app(app_id);
}

/* The status bar leaves its clock out while Home shows the time large: not
 * over the app drawer, which covers it, nor on a screen too short for it. */
static void show_bar_clock(bool show)
{
    statusbar_set_clock_visible(show || s_clock_label == NULL);
}

/* ------------------------------------------------------------------ */
/* Click handlers                                                      */
/* ------------------------------------------------------------------ */

static void close_app_drawer(void);
static void close_folder(void);
static void apps_btn_clicked_cb(lv_event_t *e);

/* Out of the tile's own click, the drawer it was in can be deleted at once,
 * so the app opened from it has the drawer's share of LVGL's pool from its
 * first object. (A deletion left to lv_obj_delete_async() would come after
 * the app: LVGL runs the newest async call first.) */
static void launch_from_grid_cb(void *arg)
{
    const char *app_id = arg;
    if (s_app_drawer) {
        lv_obj_delete(s_app_drawer);
        s_app_drawer = NULL;
        s_drawer_grid = NULL;
    }
    ESP_LOGI(TAG, "Launching app from launcher: %s", app_id);
    esp_err_t ret = app_manager_launch(app_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to launch %s: %s", app_id, esp_err_to_name(ret));
        toast_warn("App not available");
    }
}

static void app_cell_clicked_cb(lv_event_t *e)
{
    const char *app_id = (const char *)lv_obj_get_user_data(lv_event_get_target(e));
    if (!app_id) {
        ESP_LOGW(TAG, "app cell clicked: no app_id");
        return;
    }

    /* Hidden now, deleted once this click is over. */
    if (s_app_drawer) lv_obj_add_flag(s_app_drawer, LV_OBJ_FLAG_HIDDEN);
    s_drawer_visible = false;
    close_folder();
    lv_async_call(launch_from_grid_cb, (void *)app_id);
}

static void launch_pinned_cb(void *arg)
{
    const char *app_id = arg;
    ESP_LOGI(TAG, "Launching app from Home: %s", app_id);
    esp_err_t ret = app_manager_launch(app_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to launch %s: %s", app_id, esp_err_to_name(ret));
        toast_warn("App not available");
    }
}

/* Launched once this click is over: leaving Home deletes its tiles, this one
 * among them (launcher_ui_hide). */
static void pinned_clicked_cb(lv_event_t *e)
{
    const char *app_id = (const char *)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (app_id) {
        lv_async_call(launch_pinned_cb, (void *)app_id);
    }
}

static void drawer_close_btn_cb(lv_event_t *e)
{
    (void)e;
    close_app_drawer();
}

static void folder_close_btn_cb(lv_event_t *e)
{
    (void)e;
    close_folder();
}

static void folder_backdrop_clicked_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_folder) {
        close_folder();
    }
}

/* ------------------------------------------------------------------ */
/* App grid cell                                                       */
/* ------------------------------------------------------------------ */

static lv_obj_t *create_icon_cell(lv_obj_t *parent, const char *symbol,
                                  const char *name, int width, int height)
{
    const theme_colors_t *c = theme_get_colors();

    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_size(cell, width, height);
    lv_obj_set_style_bg_color(cell, c->surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(cell, c->text_secondary, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(cell, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cell, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(cell, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    /* Pressed state */
    lv_obj_set_style_bg_color(cell, c->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_STATE_PRESSED);

    /* Semantic icon */
    lv_obj_t *icon = lv_label_create(cell);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, c->text, LV_PART_MAIN);

    /* App name (small, below icon) */
    lv_obj_t *lbl = lv_label_create(cell);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, c->text_secondary, LV_PART_MAIN);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, width - 6);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    /* A name of several words wraps between them. One word too wide for the
     * cell would break mid-word ("Flashlig/ht"), so it keeps to one line and
     * is shortened instead. */
    if (!strchr(name, ' ')) {
        lv_obj_set_height(lbl, lv_font_get_line_height(&lv_font_montserrat_14));
    }

    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

    return cell;
}

static lv_obj_t *create_app_cell(lv_obj_t *parent, const char *symbol,
                                  const char *name, const char *app_id,
                                  int width, int height)
{
    lv_obj_t *cell = create_icon_cell(parent, symbol, name, width, height);
    lv_obj_set_user_data(cell, (void *)app_id);
    lv_obj_add_event_cb(cell, app_cell_clicked_cb, LV_EVENT_CLICKED, NULL);
    return cell;
}

/* ------------------------------------------------------------------ */
/* Home: the clock, the pinned apps and the All apps button            */
/* ------------------------------------------------------------------ */

/* Shared by every tile, so each costs LVGL's pool a few objects rather than
 * a style property apiece (CLAUDE.md, "LVGL's pool is 64 KB"). */
static lv_style_t s_st_tile;
static lv_style_t s_st_box;
static lv_style_t s_st_name;
static lv_style_t s_st_name_focus;
static lv_style_t s_st_bar;
static lv_style_t s_st_pressed;
static lv_style_t s_st_focus;
static lv_style_t s_st_now_band;
static lv_style_t s_st_now_row;
static lv_style_t s_st_now_rule;
static lv_style_t s_st_now_main;
static lv_style_t s_st_now_icon;
static lv_style_t s_st_now_text;
static lv_style_t s_st_now_title;
static lv_style_t s_st_now_detail;
static lv_style_t s_st_now_back;
static lv_style_t s_st_now_pressed;
static lv_style_t s_st_now_ring;
static bool       s_styles_ready = false;

/* A row of what is new in colour: a card a fifth of the accent's colour over
 * the background, as the mockup's. */
static lv_color_t now_card(const theme_colors_t *c)
{
    return lv_color_mix(c->primary, c->bg, LV_OPA_20);
}

static void init_now_styles(const theme_colors_t *c)
{
    const bool wide = s_layout.landscape;
    const lv_font_t *title_font = &lv_font_montserrat_18;
    /* A copy, so LVGL's own Montserrat 14 keeps no fallback. */
    s_detail_font = lv_font_montserrat_14;
    s_detail_font.fallback = s_mono ? &home_latin_14_mono : &home_latin_14;
    const lv_font_t *detail_font = &s_detail_font;

    /* On e-paper the rows are ruled above, between and below; in colour each
     * is a card, NOW_GAP from the next. One row alone sits in the middle. */
    const int row_h = (s_layout.now_h - (s_mono ? 2 * NOW_RULE : NOW_GAP)) / HOME_NOW_ROWS;
    lv_style_set_layout(&s_st_now_band, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_st_now_band, LV_FLEX_FLOW_COLUMN);
    lv_style_set_flex_main_place(&s_st_now_band, LV_FLEX_ALIGN_CENTER);
    lv_style_set_pad_row(&s_st_now_band, s_mono ? 0 : NOW_GAP);
    if (s_mono) {
        lv_style_set_border_side(&s_st_now_band, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM);
        lv_style_set_border_width(&s_st_now_band, NOW_RULE);
        lv_style_set_border_color(&s_st_now_band, c->text);
    }

    lv_style_set_width(&s_st_now_row, lv_pct(100));
    lv_style_set_height(&s_st_now_row, row_h);
    lv_style_set_layout(&s_st_now_row, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_st_now_row, LV_FLEX_FLOW_ROW);
    lv_style_set_flex_cross_place(&s_st_now_row, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&s_st_now_row, LV_FLEX_ALIGN_CENTER);
    lv_style_set_pad_column(&s_st_now_row, 4);
    if (!s_mono) {
        lv_style_set_bg_opa(&s_st_now_row, LV_OPA_COVER);
        lv_style_set_bg_color(&s_st_now_row, now_card(c));
        lv_style_set_radius(&s_st_now_row, 6);
        lv_style_set_pad_right(&s_st_now_row, 3);
    }
    /* On e-paper, the rule between two rows, on the second. */
    lv_style_set_border_side(&s_st_now_rule, LV_BORDER_SIDE_TOP);
    lv_style_set_border_width(&s_st_now_rule, NOW_RULE);
    lv_style_set_border_color(&s_st_now_rule, c->text);

    /* The row's own button: its icon, then its title and detail, under one
     * another on a tall screen and on one line on a wide one. The labels take
     * the button's text colour, so a press or focus inverts them on e-paper. */
    lv_style_set_flex_grow(&s_st_now_main, 1);
    lv_style_set_height(&s_st_now_main, lv_pct(100));
    lv_style_set_layout(&s_st_now_main, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_st_now_main, LV_FLEX_FLOW_ROW);
    lv_style_set_flex_cross_place(&s_st_now_main, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&s_st_now_main, LV_FLEX_ALIGN_CENTER);
    lv_style_set_pad_hor(&s_st_now_main, 4);
    lv_style_set_pad_column(&s_st_now_main, 8);
    lv_style_set_radius(&s_st_now_main, s_mono ? 3 : 6);
    lv_style_set_bg_opa(&s_st_now_main, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_now_main, s_mono ? c->bg : now_card(c));
    lv_style_set_text_color(&s_st_now_main, c->text);

    lv_style_set_text_font(&s_st_now_icon, title_font);
    lv_style_set_width(&s_st_now_icon, 22);
    lv_style_set_text_align(&s_st_now_icon, LV_TEXT_ALIGN_CENTER);

    lv_style_set_flex_grow(&s_st_now_text, 1);
    lv_style_set_height(&s_st_now_text, LV_SIZE_CONTENT);
    lv_style_set_layout(&s_st_now_text, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_st_now_text, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    /* On one line, the two sizes of text share a foot rather than a middle. */
    lv_style_set_flex_cross_place(&s_st_now_text, wide ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);
    lv_style_set_pad_column(&s_st_now_text, 8);

    /* Each cut short with "..." at the width it is given: under one another
     * the whole width, on one line the title its own and the detail the
     * rest. A label as wide as its text and cut with "..." draws "..." alone
     * (ticket 02), so the title on one line is clipped instead, and titles
     * are short. */
    lv_style_set_text_font(&s_st_now_title, title_font);
    lv_style_set_height(&s_st_now_title, lv_font_get_line_height(title_font));
    lv_style_set_text_font(&s_st_now_detail, detail_font);
    lv_style_set_height(&s_st_now_detail, lv_font_get_line_height(detail_font));
    if (wide) {
        lv_style_set_flex_grow(&s_st_now_detail, 1);
    } else {
        lv_style_set_width(&s_st_now_title, lv_pct(100));
        lv_style_set_width(&s_st_now_detail, lv_pct(100));
    }
    if (!s_mono) {
        lv_style_set_text_color(&s_st_now_detail, c->text_secondary);
    }

    /* Call back, at the end of a missed call's row. */
    const int back = wide ? 24 : 32;
    lv_style_set_width(&s_st_now_back, back);
    lv_style_set_height(&s_st_now_back, back);
    lv_style_set_radius(&s_st_now_back, wide ? 4 : 6);
    lv_style_set_bg_opa(&s_st_now_back, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_now_back, s_mono ? c->bg : lv_color_mix(c->text, c->bg, LV_OPA_20));
    lv_style_set_border_width(&s_st_now_back, s_mono ? 1 : 0);
    lv_style_set_border_color(&s_st_now_back, c->text);
    lv_style_set_text_color(&s_st_now_back, c->text);
    lv_style_set_text_font(&s_st_now_back, wide ? detail_font : title_font);

    /* Pressed, inverted on e-paper; in colour a deeper card, under which an
     * icon in the accent's colour still shows. Focused, inverted on e-paper;
     * in colour a ring inside the button, which its row would clip outside. */
    if (s_mono) {
        lv_style_set_bg_color(&s_st_now_pressed, c->text);
        lv_style_set_text_color(&s_st_now_pressed, c->bg);
    } else {
        lv_style_set_bg_color(&s_st_now_pressed, lv_color_mix(c->primary, c->bg, LV_OPA_40));
        lv_style_set_border_width(&s_st_now_ring, 2);
        lv_style_set_border_color(&s_st_now_ring, c->primary);
    }
}

static void init_home_styles(const theme_colors_t *c)
{
    lv_style_t *const all[] = {
        &s_st_tile, &s_st_box, &s_st_name, &s_st_name_focus,
        &s_st_bar, &s_st_pressed, &s_st_focus,
        &s_st_now_band, &s_st_now_row, &s_st_now_rule, &s_st_now_main,
        &s_st_now_icon, &s_st_now_text, &s_st_now_title, &s_st_now_detail,
        &s_st_now_back, &s_st_now_pressed, &s_st_now_ring,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (s_styles_ready) {
            lv_style_reset(all[i]);
        }
        lv_style_init(all[i]);
    }
    s_styles_ready = true;

    /* A pinned app: a button holding its icon in a box, and its name. */
    lv_style_set_width(&s_st_tile, s_layout.cell_w);
    lv_style_set_height(&s_st_tile, s_layout.cell_h);
    lv_style_set_bg_opa(&s_st_tile, LV_OPA_TRANSP);
    lv_style_set_layout(&s_st_tile, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_st_tile, LV_FLEX_FLOW_COLUMN);
    lv_style_set_flex_main_place(&s_st_tile, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_cross_place(&s_st_tile, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&s_st_tile, LV_FLEX_ALIGN_CENTER);
    lv_style_set_pad_row(&s_st_tile, 4);

    /* On e-paper an outline, inverted when pressed or focused. In colour a
     * filled tile, its colour set per tile, lit when pressed and ringed when
     * focused. */
    lv_style_set_width(&s_st_box, ICON_BOX);
    lv_style_set_height(&s_st_box, ICON_BOX);
    lv_style_set_radius(&s_st_box, 6);
    lv_style_set_bg_opa(&s_st_box, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_box, s_mono ? c->bg : c->surface);
    lv_style_set_border_width(&s_st_box, s_mono ? 1 : 0);
    lv_style_set_border_color(&s_st_box, c->text);
    lv_style_set_text_color(&s_st_box, c->text);
    lv_style_set_text_font(&s_st_box, &lv_font_montserrat_22);

    lv_style_set_text_font(&s_st_name, &lv_font_montserrat_14);
    lv_style_set_height(&s_st_name, lv_font_get_line_height(&lv_font_montserrat_14));
    lv_style_set_text_color(&s_st_name, c->text);
    lv_style_set_text_align(&s_st_name, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_hor(&s_st_name, NAME_PAD);
    lv_style_set_radius(&s_st_name, 3);
    if (s_mono) {
        lv_style_set_bg_opa(&s_st_name_focus, LV_OPA_COVER);
        lv_style_set_bg_color(&s_st_name_focus, c->text);
        lv_style_set_text_color(&s_st_name_focus, c->bg);
    }

    /* All apps, as wide as the grid above it. */
    lv_style_set_radius(&s_st_bar, 6);
    lv_style_set_bg_opa(&s_st_bar, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_bar, s_mono ? c->bg : c->surface);
    lv_style_set_border_width(&s_st_bar, s_mono ? 1 : 0);
    lv_style_set_border_color(&s_st_bar, c->text);
    lv_style_set_text_color(&s_st_bar, c->text);
    lv_style_set_text_font(&s_st_bar, &lv_font_montserrat_14);

    if (s_mono) {
        lv_style_set_bg_color(&s_st_pressed, c->text);
        lv_style_set_text_color(&s_st_pressed, c->bg);
        lv_style_set_bg_color(&s_st_focus, c->text);
        lv_style_set_text_color(&s_st_focus, c->bg);
    } else {
        lv_style_set_bg_color(&s_st_pressed, c->primary);
        lv_style_set_outline_width(&s_st_focus, 2);
        lv_style_set_outline_color(&s_st_focus, c->primary);
        lv_style_set_outline_pad(&s_st_focus, 2);
    }

    init_now_styles(c);
}

/* In colour the tiles take turns: mineral blue, violet, graphite. The violet
 * is darker than the focus ring's, and the graphite a fifth of the text's
 * colour over the background: the theme's surface is too near the
 * background to show. */
static lv_color_t tile_color(const theme_colors_t *c, int index)
{
    switch (index % 3) {
    case 0:  return c->secondary;
    case 1:  return lv_color_darken(c->primary, LV_OPA_30);
    default: return lv_color_mix(c->text, c->bg, LV_OPA_20);
    }
}

/* A tile's box and name show the tile's own press and keyboard focus, which
 * LVGL gives to the button alone. LVGL has set the state by the time a
 * callback hears of it. */
static void tile_state_cb(lv_event_t *e)
{
    lv_obj_t *tile = lv_event_get_current_target(e);
    const lv_state_t shown = LV_STATE_PRESSED | LV_STATE_FOCUS_KEY;
    const lv_state_t now = lv_obj_get_state(tile) & shown;
    for (uint32_t i = 0; i < lv_obj_get_child_count(tile); i++) {
        lv_obj_t *child = lv_obj_get_child(tile, (int32_t)i);
        lv_obj_remove_state(child, shown & ~now);
        lv_obj_add_state(child, now);
    }
}

static void tile_ext_draw_cb(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, FOCUS_ROOM);
}

static void create_pinned_tile(lv_obj_t *parent, const app_manifest_t *app,
                               int index, const theme_colors_t *c)
{
    /* The colour screens' focus ring is drawn outside the box, above the
     * tile's top edge: LVGL clips a child to its parent's area and what its
     * parent says it draws past that. */
    lv_obj_t *tile = lv_button_create(parent);
    lv_obj_remove_style_all(tile);
    lv_obj_add_style(tile, &s_st_tile, LV_PART_MAIN);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(tile, tile_ext_draw_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(tile);
    lv_obj_set_user_data(tile, (void *)app->id);
    lv_obj_add_event_cb(tile, pinned_clicked_cb, LV_EVENT_CLICKED, NULL);
    static const lv_event_code_t STATE_EVENTS[] = {
        LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST,
        LV_EVENT_FOCUSED, LV_EVENT_DEFOCUSED,
    };
    for (size_t i = 0; i < sizeof(STATE_EVENTS) / sizeof(STATE_EVENTS[0]); i++) {
        lv_obj_add_event_cb(tile, tile_state_cb, STATE_EVENTS[i], NULL);
    }

    /* Neither the box nor the labels take the touch: it reaches the tile. */
    lv_obj_t *box = lv_obj_create(tile);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &s_st_box, LV_PART_MAIN);
    lv_obj_add_style(box, &s_st_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(box, &s_st_focus, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (!s_mono) {
        lv_obj_set_style_bg_color(box, tile_color(c, index), LV_PART_MAIN);
    }

    /* Without the theme's label style, the glyph takes the box's colour in
     * each state. */
    lv_obj_t *glyph = lv_label_create(box);
    lv_obj_remove_style_all(glyph);
    lv_label_set_text(glyph, app_icon_symbol(app->id));
    lv_obj_center(glyph);

    /* As wide as its name, so focus inverts the name alone, but no wider than
     * the tile, past which it is cut. A label sized to its text cannot be
     * cut, so the text is measured. A name that fits only without its
     * padding goes without. */
    lv_obj_t *name = lv_label_create(tile);
    lv_obj_remove_style_all(name);
    lv_obj_add_style(name, &s_st_name, LV_PART_MAIN);
    lv_obj_add_style(name, &s_st_name_focus, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_label_set_text(name, app->name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    const lv_font_t *font = &lv_font_montserrat_14;
    int32_t text_w = lv_text_get_width(app->name, (uint32_t)strlen(app->name), font, 0);
    int32_t name_w = text_w + 2 * NAME_PAD;
    if (name_w > s_layout.cell_w) {
        lv_obj_set_style_pad_hor(name, 0, LV_PART_MAIN);
        name_w = LV_MIN(text_w, s_layout.cell_w);
    }
    lv_obj_set_width(name, name_w);
}

/* The time and the date, rewritten only when they change: on e-paper every
 * rewrite is a refresh. No date until the clock is set. */
static void set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void update_clock(void)
{
    if (s_clock_label == NULL) {
        return;
    }
    char time_buf[12];
    time_manager_format_time(time_buf, sizeof(time_buf));
    set_text_if_changed(s_clock_label, time_buf);

    char date_buf[40] = "";
    time_status_t st;
    if (time_manager_get_status(&st) == ESP_OK && st.known) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        home_format_date(tm.tm_wday, tm.tm_mday, tm.tm_mon, s_layout.landscape,
                         date_buf, sizeof(date_buf));
    }
    set_text_if_changed(s_date_label, date_buf);
}

static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_root && !lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) {
        update_clock();
    }
}

/* The time large, and the date under it, or beside it on a wide screen. */
static void create_clock(lv_obj_t *parent, const theme_colors_t *c)
{
    lv_obj_t *band = lv_obj_create(parent);
    lv_obj_remove_style_all(band);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(band, 12, s_layout.clock_y + 8);
    lv_obj_set_size(band, s_app_w - 24, s_layout.clock_h - 8);
    lv_obj_set_flex_flow(band, s_layout.landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    /* Beside the time, the date's two lines are centred on it. */
    lv_obj_set_flex_align(band, LV_FLEX_ALIGN_START,
                          s_layout.landscape ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(band, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(band, 12, LV_PART_MAIN);

    s_clock_label = lv_label_create(band);
    lv_obj_set_style_text_font(s_clock_label, s_mono ? &home_clock_44_mono : &home_clock_44,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_clock_label, c->text, LV_PART_MAIN);
    lv_label_set_text(s_clock_label, "");

    s_date_label = lv_label_create(band);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_date_label, c->text_secondary, LV_PART_MAIN);
    lv_label_set_text(s_date_label, "");

    update_clock();
    if (s_clock_timer == NULL) {
        s_clock_timer = lv_timer_create(clock_timer_cb, CLOCK_POLL_MS, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* What is new                                                          */
/* ------------------------------------------------------------------ */

/* Each reads public kernel state that the status bar or the call chip
 * already reads, and changes none of it: never rs_msg_alert_take(), which
 * takes the status bar's pop-up, nor rs_call_missed_seen(), which is the
 * Phone app's Recents'. */

/* The newest missed call and the phonebook's name for it. Asked only while a
 * call is missed: the call log is read from the card on its first use, and
 * the phonebook searches the whole card on its first. */
static void read_missed_call(home_now_in_t *in)
{
    uint32_t log_seq = rs_call_log_seq();
    if (!s_missed.read || s_missed.log_seq != log_seq) {
        s_missed.read = true;
        s_missed.log_seq = log_seq;
        s_missed.found = false;
        s_missed.named = false;
        uint32_t count = rs_call_log_count();
        for (uint32_t i = 0; i < count && !s_missed.found; i++) {
            s_missed.found = rs_call_log_get(i, &s_missed.entry) &&
                             s_missed.entry.kind == CALL_LOG_MISSED;
        }
    }
    if (!s_missed.found) {
        return;
    }

    const char *number = s_missed.entry.number;
    if (number[0] == '\0') {
        s_missed.name[0] = '\0';
    } else {
        /* The phonebook reads the card on a thread of its own and knows no
         * name until it is done, so the name is asked again whenever the
         * phonebook changes. */
        uint32_t book_seq = rs_phonebook_seq();
        if (!s_missed.named || s_missed.book_seq != book_seq) {
            s_missed.named = true;
            s_missed.book_seq = book_seq;
            if (!rs_phonebook_name_for(number, s_missed.name, sizeof(s_missed.name))) {
                s_missed.name[0] = '\0';
            }
        }
    }
    in->missed_number = number;
    in->missed_name = s_missed.name;

    /* In local time, as the status bar's clock: localtime_r() reads the
     * zone the kernel keeps in TZ. */
    if (s_missed.entry.time != 0) {
        time_t when = (time_t)s_missed.entry.time;
        struct tm tm;
        if (localtime_r(&when, &tm)) {
            in->missed_hour = (int8_t)tm.tm_hour;
            in->missed_minute = (int8_t)tm.tm_min;
        }
        time_status_t st;
        if (time_manager_get_status(&st) == ESP_OK) {
            in->format_24h = st.format_24h;
        }
    }
}

static home_tailnet_t read_tailnet(void)
{
    if (!tailnet_is_enabled()) {
        return HOME_TAILNET_QUIET;
    }
    tailnet_status_t st;
    if (tailnet_get_status(&st) != ESP_OK) {
        return HOME_TAILNET_QUIET;
    }
    switch (st.state) {
    case TAILNET_NEEDS_LOGIN:    return HOME_TAILNET_LOGIN;
    case TAILNET_NEEDS_APPROVAL: return HOME_TAILNET_APPROVAL;
    default:                     return HOME_TAILNET_QUIET;
    }
}

static void read_now(home_now_in_t *in)
{
    memset(in, 0, sizeof(*in));
    in->missed_hour = -1;
    in->format_24h = true;
    in->sms_unread = rs_sms_unread_count();
    in->mesh_unread = rs_mesh_unread_count();
    in->missed_calls = rs_call_missed_count();
    if (in->missed_calls > 0) {
        read_missed_call(in);
    }
    in->tailnet = read_tailnet();
    in->mesh_running = rs_mesh_is_running();
    in->mesh_contacts = in->mesh_running ? rs_mesh_get_contact_count() : 0;
}

static const char *now_symbol(home_now_kind_t kind)
{
    switch (kind) {
    case HOME_NOW_MESSAGES:    return LV_SYMBOL_ENVELOPE;
    case HOME_NOW_MISSED_CALL: return LV_SYMBOL_CALL;
    case HOME_NOW_TAILNET:     return LV_SYMBOL_SHUFFLE;   /* as in the drawer */
    case HOME_NOW_MESH:        return LV_SYMBOL_LOOP;      /* as the quick panel's LoRa */
    case HOME_NOW_CAUGHT_UP:
    default:                   return LV_SYMBOL_OK;
    }
}

/* What a tap on the row opens, as the status bar's envelope and the call
 * chip do; NULL for a row that is not a button. */
static const char *now_app(home_now_kind_t kind)
{
    switch (kind) {
    case HOME_NOW_MESSAGES:
    case HOME_NOW_MESH:        return "com.thistle.messenger";
    case HOME_NOW_MISSED_CALL: return "com.thistle.phone";
    case HOME_NOW_TAILNET:     return "com.thistle.tailscale";
    case HOME_NOW_CAUGHT_UP:
    default:                   return NULL;
    }
}

/* In colour the icon takes the accent of what it is about. */
static lv_color_t now_icon_color(const theme_colors_t *c, home_now_kind_t kind)
{
    switch (kind) {
    case HOME_NOW_MESSAGES:    return c->primary;
    case HOME_NOW_MISSED_CALL: return c->secondary;
    default:                   return c->text;
    }
}

/* Launched once this click is over, as a pinned app is: leaving Home deletes
 * its rows, this one among them. */
static void now_clicked_cb(lv_event_t *e)
{
    const char *app_id = (const char *)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (app_id) {
        lv_async_call(launch_pinned_cb, (void *)app_id);
    }
}

/* The call screen opens over Home as the kernel places the call, so nothing
 * of Home is deleted here. */
static void call_back_clicked_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (index < 0 || index >= s_now_count) {
        return;
    }
    const char *number = s_now_rows[index].call_back;
    if (number[0] == '\0') {
        return;
    }
    esp_err_t err = rs_call_dial(number);
    ESP_LOGI(TAG, "call back %s: %s", number, esp_err_to_name(err));
    switch (err) {
    case ESP_OK:
        break;
    case ESP_ERR_NOT_SUPPORTED:
        toast_warn("This device cannot make calls");
        break;
    case ESP_ERR_INVALID_STATE:
        toast_warn("Calls are not ready yet");
        break;
    default:
        toast_warn("The call could not be placed");
        break;
    }
}

/* Hidden or shown only on a change: LVGL redraws an object it is told to
 * show though it was showing, which on e-paper is a refresh. */
static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) != hidden) {
        lv_obj_update_flag(obj, LV_OBJ_FLAG_HIDDEN, hidden);
    }
}

/* A button about to be hidden or made inert hands the keyboard's focus on. */
static void leave_focus(lv_obj_t *obj)
{
    lv_group_t *group = lv_obj_get_group(obj);
    if (group && lv_group_get_focused(group) == obj) {
        lv_group_focus_next(group);
    }
}

static lv_obj_t *now_label(lv_obj_t *parent, const lv_style_t *style)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_obj_add_style(label, style, LV_PART_MAIN);
    lv_label_set_text(label, "");
    return label;
}

/* A row, hidden until update_now() fills it. Its buttons join the keyboard's
 * group as they are made, so Home's rows come before its pinned apps. */
static void create_now_slot(lv_obj_t *band, int index)
{
    lv_style_t *focus = s_mono ? &s_st_focus : &s_st_now_ring;
    now_slot_t *slot = &s_now[index];

    slot->row = lv_obj_create(band);
    lv_obj_remove_style_all(slot->row);
    lv_obj_add_style(slot->row, &s_st_now_row, LV_PART_MAIN);
    if (s_mono && index > 0) {
        lv_obj_add_style(slot->row, &s_st_now_rule, LV_PART_MAIN);
    }
    lv_obj_clear_flag(slot->row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(slot->row, LV_OBJ_FLAG_HIDDEN);

    slot->main = lv_button_create(slot->row);
    lv_obj_remove_style_all(slot->main);
    lv_obj_add_style(slot->main, &s_st_now_main, LV_PART_MAIN);
    lv_obj_add_style(slot->main, &s_st_now_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(slot->main, focus, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(slot->main, now_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* Neither the labels nor the box holding title and detail take the
     * touch: it reaches the button. */
    slot->icon = now_label(slot->main, &s_st_now_icon);
    lv_obj_t *text = lv_obj_create(slot->main);
    lv_obj_remove_style_all(text);
    lv_obj_add_style(text, &s_st_now_text, LV_PART_MAIN);
    lv_obj_clear_flag(text, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    slot->title = now_label(text, &s_st_now_title);
    slot->detail = now_label(text, &s_st_now_detail);
    lv_label_set_long_mode(slot->title, s_layout.landscape ? LV_LABEL_LONG_CLIP : LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(slot->detail, LV_LABEL_LONG_DOT);

    slot->back = lv_button_create(slot->row);
    lv_obj_remove_style_all(slot->back);
    lv_obj_add_style(slot->back, &s_st_now_back, LV_PART_MAIN);
    lv_obj_add_style(slot->back, &s_st_now_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(slot->back, focus, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_user_data(slot->back, (void *)(intptr_t)index);
    lv_obj_add_event_cb(slot->back, call_back_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(slot->back, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *glyph = lv_label_create(slot->back);
    lv_obj_remove_style_all(glyph);
    lv_label_set_text(glyph, LV_SYMBOL_CALL);
    lv_obj_center(glyph);
}

/* Fill the rows with `rows`, touching only what differs from what they
 * show: on e-paper every change is a refresh of its area. */
static void show_now(const home_now_row_t *rows, int n)
{
    const theme_colors_t *c = theme_get_colors();
    for (int i = 0; i < HOME_NOW_ROWS; i++) {
        now_slot_t *slot = &s_now[i];
        if (i >= n) {
            leave_focus(slot->main);
            leave_focus(slot->back);
            set_hidden(slot->row, true);
            continue;
        }
        const home_now_row_t *row = &rows[i];
        if (i >= s_now_count || s_now_rows[i].kind != row->kind) {
            const char *app_id = now_app(row->kind);
            set_text_if_changed(slot->icon, now_symbol(row->kind));
            if (!s_mono) {
                lv_obj_set_style_text_color(slot->icon, now_icon_color(c, row->kind), LV_PART_MAIN);
            }
            if (app_id == NULL) {
                leave_focus(slot->main);
            }
            lv_obj_set_user_data(slot->main, (void *)app_id);
            lv_obj_set_state(slot->main, LV_STATE_DISABLED, app_id == NULL);
        }
        set_text_if_changed(slot->title, row->title);
        set_text_if_changed(slot->detail, row->detail);
        bool back = row->call_back[0] != '\0';
        if (!back) {
            leave_focus(slot->back);
        }
        set_hidden(slot->back, !back);
        set_hidden(slot->row, false);
    }
}

static void log_now(const home_now_row_t *rows, int n)
{
    char line[2 * (HOME_NOW_TITLE_MAX + HOME_NOW_DETAIL_MAX + 8)];
    size_t at = 0;
    line[0] = '\0';
    for (int i = 0; i < n && at < sizeof(line); i++) {
        int w = snprintf(line + at, sizeof(line) - at, "%s%s (%s)",
                         i ? " | " : "", rows[i].title, rows[i].detail);
        if (w < 0) {
            break;
        }
        at += (size_t)w;
    }
    ESP_LOGI(TAG, "now: %s", line);
}

/* Read what is new, and redraw the rows if it changed. */
static void update_now(void)
{
    if (s_now_band == NULL) {
        return;
    }
    home_now_in_t in;
    read_now(&in);
    home_now_row_t rows[HOME_NOW_ROWS];
    int n = home_now_rows(&in, rows, HOME_NOW_ROWS);
    if (n == s_now_count && memcmp(rows, s_now_rows, sizeof(rows)) == 0) {
        return;
    }
    show_now(rows, n);
    memcpy(s_now_rows, rows, sizeof(rows));
    s_now_count = n;
    log_now(rows, n);
}

static void now_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_home) {
        update_now();
    }
}

/* The rows of what is new, under the clock. */
static void create_now(lv_obj_t *parent)
{
    s_now_band = lv_obj_create(parent);
    lv_obj_remove_style_all(s_now_band);
    lv_obj_add_style(s_now_band, &s_st_now_band, LV_PART_MAIN);
    lv_obj_clear_flag(s_now_band, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_now_band, HOME_MARGIN, s_layout.now_y);
    lv_obj_set_size(s_now_band, s_app_w - 2 * HOME_MARGIN, s_layout.now_h);
    for (int i = 0; i < HOME_NOW_ROWS; i++) {
        create_now_slot(s_now_band, i);
    }

    s_now_count = -1;
    update_now();
    if (s_now_timer == NULL) {
        s_now_timer = lv_timer_create(now_timer_cb, NOW_POLL_MS, NULL);
    }
}

/* The apps pinned to Home, centred in their band. */
static void create_pinned_grid(lv_obj_t *parent, const theme_colors_t *c)
{
    int slots = s_layout.cols * s_layout.rows;
    if (slots > HOME_MAX_PINS) {
        slots = HOME_MAX_PINS;
    }
    if (slots == 0) {
        return;
    }

    const app_manifest_t *apps[32];
    const char *ids[32];
    int count = app_manager_list_apps(apps, 32);
    for (int i = 0; i < count; i++) {
        ids[i] = apps[i]->id;
    }
    int picked[HOME_MAX_PINS];
    int n = home_pick_pins(s_pin_preference,
                           sizeof(s_pin_preference) / sizeof(s_pin_preference[0]),
                           ids, (size_t)count, pin_eligible, picked, slots);

    /* Room above and below the tiles for the focus ring: across, the icons
     * are well inside their cells. */
    const int grid_w = s_layout.cols * s_layout.cell_w;
    const int grid_h = s_layout.rows * s_layout.cell_h;
    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_ver(grid, FOCUS_ROOM, LV_PART_MAIN);
    lv_obj_set_size(grid, grid_w, grid_h + 2 * FOCUS_ROOM);
    lv_obj_set_pos(grid, (s_app_w - grid_w) / 2,
                   s_layout.grid_y + (s_layout.grid_h - grid_h) / 2 - FOCUS_ROOM);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    for (int i = 0; i < n; i++) {
        create_pinned_tile(grid, apps[picked[i]], i, c);
    }
}

/* All apps, along the bottom. */
static void create_apps_bar(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    s_apps_btn = btn;
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &s_st_bar, LV_PART_MAIN);
    lv_obj_add_style(btn, &s_st_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(btn, &s_st_focus, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_pos(btn, HOME_MARGIN, s_layout.bar_y + 4);
    lv_obj_set_size(btn, s_app_w - 2 * HOME_MARGIN, s_layout.bar_h - 8);
    lv_obj_add_event_cb(btn, apps_btn_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_remove_style_all(lbl);
    lv_label_set_text(lbl, LV_SYMBOL_LIST "  All apps");
    lv_obj_center(lbl);
}

/* ------------------------------------------------------------------ */
/* App drawer                                                          */
/* ------------------------------------------------------------------ */

static void populate_app_drawer(void)
{
    const app_manifest_t *apps[32];
    int count = app_manager_list_apps(apps, 32);

    /* As many CELL_SIZE columns as fit, then stretched to fill the row: the
     * spare pixels are worth more to a label than to the right margin. */
    const int gap = 6;
    int inner_w = s_app_w - 2 * gap;
    int cols = (inner_w + gap) / (CELL_SIZE + gap);
    if (cols < 1) cols = 1;
    int cell_w = (inner_w - (cols - 1) * gap) / cols;

    for (int i = 0; i < count; i++) {
        /* Skip the launcher itself */
        if (strcmp(apps[i]->id, "com.thistle.launcher") == 0) continue;

        if (is_launcher(apps[i]->id)) continue;

        create_app_cell(s_drawer_grid, app_icon_symbol(apps[i]->id),
                        apps[i]->name, apps[i]->id, cell_w, CELL_SIZE);
    }
}

static void open_app_drawer(void)
{
    const theme_colors_t *colors = theme_get_colors();

    if (!s_app_drawer) {
        /* Full-screen overlay on top of the home screen */
        s_app_drawer = lv_obj_create(s_root);
        lv_obj_set_size(s_app_drawer, s_app_w, s_app_h);
        lv_obj_set_pos(s_app_drawer, 0, 0);
        lv_obj_set_style_bg_color(s_app_drawer, colors->bg, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_app_drawer, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_app_drawer, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_app_drawer, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(s_app_drawer, 0, LV_PART_MAIN);
        lv_obj_clear_flag(s_app_drawer, LV_OBJ_FLAG_SCROLLABLE);

        /* --- Header bar --- */
        lv_obj_t *header = lv_obj_create(s_app_drawer);
        lv_obj_set_size(header, s_app_w, 30);
        lv_obj_set_pos(header, 0, 0);
        lv_obj_set_style_bg_color(header, colors->surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_border_color(header, colors->text_secondary, LV_PART_MAIN);
        lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(header, 4, LV_PART_MAIN);
        lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
        lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(header);
        lv_label_set_text(title, "All Apps");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(title, colors->text, LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 4, 0);

        lv_obj_t *close_btn = lv_button_create(header);
        lv_obj_set_size(close_btn, 22, 22);
        lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_set_style_bg_color(close_btn, colors->surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(close_btn, colors->text_secondary, LV_PART_MAIN);
        lv_obj_set_style_border_width(close_btn, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(close_btn, 4, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(close_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(close_btn, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(close_btn, colors->primary, LV_STATE_PRESSED);
        lv_obj_add_event_cb(close_btn, drawer_close_btn_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *close_lbl = lv_label_create(close_btn);
        lv_label_set_text(close_lbl, "X");
        lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(close_lbl, colors->text, LV_PART_MAIN);
        lv_obj_center(close_lbl);

        /* --- Scrollable grid area --- */
        lv_obj_t *grid_scroll = lv_obj_create(s_app_drawer);
        lv_obj_set_pos(grid_scroll, 0, 30);
        lv_obj_set_size(grid_scroll, s_app_w, s_app_h - 30);
        lv_obj_set_style_bg_opa(grid_scroll, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(grid_scroll, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(grid_scroll, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_row(grid_scroll, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_column(grid_scroll, 6, LV_PART_MAIN);
        lv_obj_set_style_radius(grid_scroll, 0, LV_PART_MAIN);
        lv_obj_set_scroll_dir(grid_scroll, LV_DIR_VER);

        /* 4-column flex wrap */
        lv_obj_set_flex_flow(grid_scroll, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(grid_scroll,
                              LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_START);

        s_drawer_grid = grid_scroll;

        populate_app_drawer();
    }

    lv_obj_clear_flag(s_app_drawer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_app_drawer);
    s_drawer_visible = true;
    show_bar_clock(true);
}

/* Deleted rather than hidden: its tiles take a share of LVGL's 64 KB pool
 * that an app opened over it needs (CLAUDE.md). The Files app, opened from
 * here over a card of nine folders, once ran the pool dry with all nineteen
 * tiles still standing beneath it. Asynchronously, as the close can come
 * from a click inside it. Opening the drawer builds it again. */
static void close_app_drawer(void)
{
    if (s_app_drawer) {
        lv_obj_delete_async(s_app_drawer);
        s_app_drawer = NULL;
        s_drawer_grid = NULL;
    }
    s_drawer_visible = false;
    show_bar_clock(false);
}

/* ------------------------------------------------------------------ */
/* Tools folder                                                        */
/* ------------------------------------------------------------------ */

static void open_folder(void)
{
    const theme_colors_t *colors = theme_get_colors();

    if (!s_folder) {
        int panel_w = s_app_w > 250 ? 230 : s_app_w - 24;
        int panel_h = s_app_h > 250 ? 184 : s_app_h - 28;

        s_folder = lv_obj_create(s_root);
        lv_obj_set_size(s_folder, s_app_w, s_app_h);
        lv_obj_set_pos(s_folder, 0, 0);
        lv_obj_set_style_bg_color(s_folder, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_folder, LV_OPA_60, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_folder, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_folder, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(s_folder, 0, LV_PART_MAIN);
        lv_obj_clear_flag(s_folder, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_folder, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_folder, folder_backdrop_clicked_cb,
                            LV_EVENT_CLICKED, NULL);

        lv_obj_t *panel = lv_obj_create(s_folder);
        lv_obj_set_size(panel, panel_w, panel_h);
        lv_obj_center(panel);
        lv_obj_set_style_bg_color(panel, colors->surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(panel, colors->primary, LV_PART_MAIN);
        lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(panel, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_all(panel, 6, LV_PART_MAIN);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(panel);
        lv_label_set_text(title, "Tools");
        lv_obj_set_style_text_color(title, colors->text, LV_PART_MAIN);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 2);

        lv_obj_t *close_btn = lv_button_create(panel);
        lv_obj_set_size(close_btn, 24, 24);
        lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -2);
        lv_obj_set_style_bg_color(close_btn, colors->surface, LV_PART_MAIN);
        lv_obj_set_style_border_width(close_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(close_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(close_btn, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(close_btn, folder_close_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *close_label = lv_label_create(close_btn);
        lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(close_label, colors->text, LV_PART_MAIN);
        lv_obj_center(close_label);

        s_folder_grid = lv_obj_create(panel);
        lv_obj_set_pos(s_folder_grid, 0, 28);
        lv_obj_set_size(s_folder_grid, panel_w - 12, panel_h - 40);
        lv_obj_set_style_bg_opa(s_folder_grid, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_folder_grid, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_folder_grid, 2, LV_PART_MAIN);
        lv_obj_set_style_pad_row(s_folder_grid, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_column(s_folder_grid, 4, LV_PART_MAIN);
        lv_obj_set_flex_flow(s_folder_grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(s_folder_grid, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

        const app_manifest_t *apps[32];
        int count = app_manager_list_apps(apps, 32);
        int cell_w = (panel_w - 28) / 3;
        int cell_h = panel_h > 170 ? 66 : 58;
        for (int i = 0; i < count; i++) {
            if (!is_tool_app(apps[i]->id)) continue;
            create_app_cell(s_folder_grid, app_icon_symbol(apps[i]->id),
                            apps[i]->name, apps[i]->id, cell_w, cell_h);
        }
    }

    close_app_drawer();
    lv_obj_clear_flag(s_folder, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_folder);
    s_folder_visible = true;
}

static void close_folder(void)
{
    if (s_folder) lv_obj_add_flag(s_folder, LV_OBJ_FLAG_HIDDEN);
    s_folder_visible = false;
}

static void folder_btn_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (s_folder_visible) close_folder();
    else open_folder();
}

/* ------------------------------------------------------------------ */
/* "Apps" button callback                                              */
/* ------------------------------------------------------------------ */

static void apps_btn_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (s_drawer_visible) {
        close_app_drawer();
    } else {
        close_folder();
        open_app_drawer();
    }
}

/* Back to the bare home screen, from the app drawer or a folder. Also the
 * Home key's handler, for a press made while the launcher is in front. */
static void close_overlays(void)
{
    if (s_drawer_visible) {
        close_app_drawer();
    }
    if (s_folder_visible) {
        close_folder();
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t launcher_ui_create(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "creating ThistleOS launcher UI");

    if (parent == NULL) {
        parent = lv_scr_act();
    }

    /* Read actual dimensions from parent */
    lv_obj_update_layout(parent);
    s_app_w = lv_obj_get_width(parent);
    s_app_h = lv_obj_get_height(parent);
    if (s_app_w == 0) s_app_w = 240;  /* fallback */
    if (s_app_h == 0) s_app_h = 296;

    const theme_colors_t *colors = theme_get_colors();

    const hal_registry_t *reg = hal_get_registry();
    s_mono = reg && reg->display && reg->display->type == HAL_DISPLAY_TYPE_EPAPER;
    home_layout((int16_t)s_app_w, (int16_t)s_app_h, &s_layout);
    init_home_styles(colors);

    /* Root container — fills the entire app area, in the theme's background:
     * the app area under it is not painted. */
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, colors->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Home itself is built when it comes to the front (launcher_ui_show).
     * Overlays start hidden and are created lazily on first open. */
    s_home          = NULL;
    s_clock_label   = NULL;
    s_date_label    = NULL;
    s_now_band      = NULL;
    s_now_count     = -1;
    s_apps_btn      = NULL;
    s_app_drawer    = NULL;
    s_drawer_grid   = NULL;
    s_folder        = NULL;
    s_folder_grid   = NULL;
    s_drawer_visible = false;
    s_folder_visible = false;

    ui_manager_set_home_handler(close_overlays);

    return ESP_OK;
}

/* Home, top to bottom: the clock, what is new, the pinned apps and All apps
 * (home_core.h); a band the screen has no room for is left out. Built when
 * Home comes to the front and deleted when it
 * leaves, so an app opened over it has Home's share of LVGL's pool, as one
 * opened from the drawer has the drawer's: Files, over a card of twenty
 * folders, ran the pool dry with Home's tiles still standing beneath it. */
static void build_home(void)
{
    if (s_home || !s_root) {
        return;
    }
    const theme_colors_t *colors = theme_get_colors();
    s_home = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_home);
    lv_obj_set_size(s_home, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_home, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (s_layout.clock_h > 0) {
        create_clock(s_home, colors);
    }
    /* Before the pinned apps, so the keyboard reaches the rows first. */
    if (s_layout.now_h > 0) {
        create_now(s_home);
    }
    create_pinned_grid(s_home, colors);
    create_apps_bar(s_home);
}

/* Never from inside one of Home's own events: a pinned app, or a row, launches
 * once its click is over (pinned_clicked_cb, now_clicked_cb). */
static void destroy_home(void)
{
    if (s_home) {
        lv_obj_delete(s_home);
        s_home = NULL;
    }
    s_clock_label = NULL;
    s_date_label  = NULL;
    s_now_band    = NULL;
    memset(s_now, 0, sizeof(s_now));
    s_now_count   = -1;
    s_apps_btn    = NULL;
}

static bool within(lv_obj_t *obj, const lv_obj_t *ancestor)
{
    for (; obj; obj = lv_obj_get_parent(obj)) {
        if (obj == ancestor) {
            return true;
        }
    }
    return false;
}

/* The keyboard's focus starts on All apps, Home's last button, unseen until
 * a key is pressed, so the first Tab goes round to the first: a row of what
 * is new, when one is a button. LVGL focuses the first button to join an
 * empty group, and the first Tab moved on from it unseen; or, as the rows
 * join hidden, nothing, and then a keypad does nothing at all. Focus that is
 * outside Home stays where it is. */
static void focus_home(void)
{
    lv_group_t *group = lv_group_get_default();
    if (group == NULL || s_apps_btn == NULL) {
        return;
    }
    lv_obj_t *focused = lv_group_get_focused(group);
    if (focused == NULL || within(focused, s_home)) {
        lv_group_focus_obj(s_apps_btn);
    }
}

void launcher_ui_show(void)
{
    build_home();
    if (s_root) {
        lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
    update_clock();
    update_now();
    show_bar_clock(false);
    focus_home();
}

void launcher_ui_hide(void)
{
    /* Close transient layers so returning always shows the home screen. */
    close_overlays();
    destroy_home();
    if (s_root) {
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
    show_bar_clock(true);
}
