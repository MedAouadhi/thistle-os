/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Podcasts app UI
 *
 * Pages on a stack, only the one on top built, each a full-screen container
 * under one root:
 *
 *   Home        what is playing, then the sections
 *   List        Trending, Categories and a category's shows, New episodes,
 *               Starred, Saved, Recently viewed, Continue listening
 *   Search      a query, by term, title, person or music, and its results
 *   Podcast     what it is, starred or not, and its episodes
 *   Episode     what it is; play, save to the card, star
 *   Now playing pod_ui_player.c
 *   Setup       the PodcastIndex key and secret
 *
 * Going back rebuilds the page beneath from what it had fetched, with the
 * row it had selected, so only one page's widgets are ever in LVGL's 64 KB
 * pool. A list is one lv_table, as in the Reader: a row costs one string.
 *
 * PodcastIndex is asked on a thread of its own, one request a page, the
 * Assistant's way: the job has an atomic state, a 250 ms timer polls it, and
 * a page that goes abandons its job, which the thread then frees.
 *
 * The player and the downloads are not the pages': they go on when the app
 * is left (see pod_player.h).
 */
#include "podcasts/podcasts_app.h"
#include "podcasts/pod_api.h"
#include "podcasts/pod_cred.h"
#include "podcasts/pod_download.h"
#include "podcasts/pod_library.h"
#include "podcasts/pod_net.h"
#include "podcasts/pod_player.h"
#include "podcasts/pod_text.h"
#include "podcasts/pod_ui.h"

#include "hal/board.h"
#include "hal/sdcard_path.h"
#include "thistle/app_manager.h"
#include "ui/arabic_shape.h"
#include "ui/manager.h"
#include "ui/theme.h"
#include "ui/toast.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/misc/lv_text_private.h" /* lv_text_get_next_line(), as the table uses */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "podcasts_ui";

LV_FONT_DECLARE(reader_font_16);
LV_FONT_DECLARE(reader_font_16_mono);
LV_FONT_DECLARE(pod_symbols_16);
LV_FONT_DECLARE(pod_symbols_16_mono);

#define LIBRARY_DIR   THISTLE_SDCARD "/data/podcasts"
#define STACK_MAX     10
#define MAX_ROWS      128
#define RESULTS_MAX   50       /* asked of PodcastIndex at a time */
#define SECTION_MAX   50       /* of a library list */
#define CELL_MAX      640
#define TICK_MS       250
#define JOB_STACK     8192
#define JOB_PRIORITY  5
#define CELL_PAD_W    8
#define LINE_SPACE    (-2)     /* as the Reader: DejaVu's line is generous */
#define STAR          "\xE2\x98\x85"
#define STAR_HOLLOW   "\xE2\x98\x86"
#define DOT           " \xC2\xB7 "
#define ELLIPSIS      "\xE2\x80\xA6"

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    PAGE_HOME, PAGE_LIST, PAGE_SEARCH, PAGE_PODCAST, PAGE_EPISODE, PAGE_PLAYER, PAGE_SETUP,
} page_kind_t;

typedef enum {
    LIST_TRENDING, LIST_CATEGORIES, LIST_CATEGORY, LIST_NEW,
    LIST_STARRED, LIST_SAVED, LIST_VIEWED, LIST_CONTINUE,
} list_kind_t;

/* What a row of a list is. Notes, headings and a podcast's about row are
 * read, not chosen: the selection passes over them. */
typedef enum {
    ROW_NOTE, ROW_HEADING, ROW_ABOUT, ROW_FEED, ROW_EPISODE, ROW_CATEGORY, ROW_HOME, ROW_RETRY,
} row_kind_t;

typedef enum {
    HOME_PLAYING, HOME_SEARCH, HOME_TRENDING, HOME_CATEGORIES, HOME_NEW, HOME_STARRED,
    HOME_SAVED, HOME_VIEWED, HOME_CONTINUE, HOME_SETUP,
} home_item_t;

typedef enum { JOB_RUNNING, JOB_DONE, JOB_ABANDONED } job_state_t;
typedef enum { WANT_FEEDS, WANT_EPISODES, WANT_CATEGORIES } job_want_t;

typedef struct {
    atomic_int state;
    job_want_t want;
    int        max;
    char      *path;
    void      *items;
    int        count;          /* -1 when it failed */
    char       error[96];
} job_t;

typedef struct {
    page_kind_t       kind;
    list_kind_t       list;
    char              title[64];
    char              query[96];   /* a search's, or a category's name */
    pod_search_mode_t mode;
    bool              clean;
    pod_feed_t       *feeds;
    int               feed_count;
    pod_episode_t    *episodes;
    int               episode_count;
    pod_category_t   *categories;
    int               category_count;
    bool              loaded;      /* has what it shows, or knows why not */
    bool              offline;     /* a podcast's: the library's episodes, the fetch having failed */
    char              error[96];
    pod_feed_t       *feed;        /* a podcast page's */
    pod_episode_t    *episode;     /* an episode page's */
    job_t            *job;
    int               selected;    /* the row to select when shown again */
} page_t;

static EXT_RAM_BSS_ATTR struct {
    lv_obj_t   *root;
    lv_obj_t   *screen;            /* the page shown */
    lv_obj_t   *table;             /* its list, when it has one */
    lv_obj_t   *star;              /* its title bar's star */
    page_t     *stack[STACK_MAX];
    int         depth;
    struct {
        uint8_t kind;
        int16_t index;
    } rows[MAX_ROWS];
    int         row_count;
    bool        filling;           /* a list's rows are being set, not chosen */
    lv_timer_t *tick;
    uint32_t    ticks;
    int32_t     w, h;

    /* Search */
    lv_obj_t   *query_ta;
    lv_obj_t   *mode_btn;
    lv_obj_t   *clean_btn;
    /* Episode */
    lv_obj_t   *play_btn;
    lv_obj_t   *save_btn;
    lv_obj_t   *status_lbl;
    int         confirm_ticks;     /* a Delete waiting for its second tap */
    /* Setup */
    lv_obj_t   *key_ta;
    lv_obj_t   *secret_ta;
    /* Home: what the Now playing row showed */
    pod_player_state_t home_state;
    int64_t     home_episode;
} s_ui;

static lv_font_t s_text_font;
static lv_font_t s_symbol_font;

/* Copies for the rows' library lookups, the LVGL task's stack being 8 KB. */
static EXT_RAM_BSS_ATTR pod_lib_episode_t s_known;
static EXT_RAM_BSS_ATTR pod_lib_episode_t s_lib_list[SECTION_MAX];
static EXT_RAM_BSS_ATTR pod_feed_t s_feed_list[SECTION_MAX];

static void show_top(void);
static void fill_rows(page_t *p);
static void start_load(page_t *p);
static void open_row(uint32_t row);

static void *psram_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(size);
}

static void *psram_calloc(size_t n, size_t size)
{
    void *p = psram_alloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

static int64_t now(void)
{
    return (int64_t)time(NULL);
}

static page_t *top(void)
{
    return s_ui.depth ? s_ui.stack[s_ui.depth - 1] : NULL;
}

/* ------------------------------------------------------------------ */
/* Shared with pod_ui_player.c                                          */
/* ------------------------------------------------------------------ */

bool pod_ui_epaper(void)
{
    const hal_registry_t *reg = hal_get_registry();
    return reg && reg->display && reg->display->type == HAL_DISPLAY_TYPE_EPAPER;
}

const lv_font_t *pod_ui_font(void)
{
    return &s_text_font;
}

int32_t pod_ui_width(void)
{
    return s_ui.w;
}

int32_t pod_ui_height(void)
{
    return s_ui.h;
}

/* The text with any Arabic shaped, for the caller to free(); NULL when it
 * has none, or there is no memory. */
static char *shaped(const char *text)
{
    size_t len = strlen(text);
    return arabic_present(text, len) ? arabic_shape(text, len, NULL, 0, NULL) : NULL;
}

void pod_ui_set_text(lv_obj_t *label, const char *text)
{
    if (!label) return;
    char *s = shaped(text);
    const char *t = s ? s : text;
    /* The e-paper redraws whatever is set, changed or not. */
    if (strcmp(lv_label_get_text(label), t) != 0) lv_label_set_text(label, t);
    free(s);
}

void pod_ui_focus(lv_obj_t *obj)
{
    lv_group_t *grp = lv_group_get_default();
    if (!grp || !obj) return;
    if (lv_obj_get_group(obj) != grp) lv_group_add_obj(grp, obj);
    lv_group_focus_obj(obj);
}

bool pod_ui_from_touch(void)
{
    lv_indev_t *indev = lv_indev_active();
    return indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER;
}

static void title_clicked_cb(lv_event_t *e)
{
    (void)e;
    pod_ui_back();
}

lv_obj_t *pod_ui_page(const char *title, lv_obj_t **right_btn)
{
    const theme_colors_t *clr = theme_get_colors();

    lv_obj_t *page = lv_obj_create(s_ui.root);
    lv_obj_set_size(page, s_ui.w, s_ui.h);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, clr->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(page, 0, LV_PART_MAIN);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(page);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, s_ui.w, POD_UI_TITLE_H);
    lv_obj_set_style_bg_color(bar, clr->surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(bar, POD_UI_PAD, LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, clr->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    if (s_ui.depth > 1) {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(bar, title_clicked_cb, LV_EVENT_CLICKED, NULL);
    }

    int32_t right_w = right_btn ? 40 : 0;
    lv_obj_t *label = lv_label_create(bar);
    char text[96];
    snprintf(text, sizeof text, "%s%s", s_ui.depth > 1 ? "< " : "", title);
    lv_label_set_text(label, "");
    pod_ui_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, s_ui.w - POD_UI_PAD * 2 - right_w);
    lv_obj_set_style_text_font(label, &s_text_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, clr->text, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    if (right_btn) {
        lv_obj_t *btn = lv_button_create(bar);
        lv_obj_set_size(btn, right_w, POD_UI_TITLE_H - 2);
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, -1);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, clr->primary, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(btn, clr->primary, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, clr->text, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, clr->bg, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_text_color(btn, clr->bg, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_t *mark = lv_label_create(btn);
        lv_obj_remove_style_all(mark);    /* its colour is the button's */
        lv_label_set_text(mark, "");
        lv_obj_set_style_text_font(mark, &s_text_font, LV_PART_MAIN);
        lv_obj_center(mark);
        *right_btn = btn;
    }
    return page;
}

lv_obj_t *pod_ui_button_label(lv_obj_t *button)
{
    return lv_obj_get_child(button, 0);
}

lv_obj_t *pod_ui_button(lv_obj_t *parent, const char *text, const lv_font_t *font,
                        lv_event_cb_t clicked, void *user_data)
{
    const theme_colors_t *clr = theme_get_colors();
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, clr->surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, clr->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, clr->radius, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(btn, 4, LV_PART_MAIN);
    /* Pressed or chosen with the keys, inverted, as the lists' rows. */
    lv_obj_set_style_bg_color(btn, clr->primary, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, clr->primary, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, clr->surface, LV_PART_MAIN | LV_STATE_DISABLED);
    /* The label takes its colour from the button, so it inverts with it. */
    lv_obj_set_style_text_color(btn, clr->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, clr->bg, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(btn, clr->bg, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, clr->text_secondary, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_t *label = lv_label_create(btn);
    /* The theme gives every label a colour of its own, which would keep it
     * from following the button's. */
    lv_obj_remove_style_all(label);
    lv_label_set_text(label, "");
    pod_ui_set_text(label, text);
    lv_obj_set_style_text_font(label, font ? font : &s_text_font, LV_PART_MAIN);
    lv_obj_center(label);
    if (clicked) lv_obj_add_event_cb(btn, clicked, LV_EVENT_CLICKED, user_data);
    return btn;
}

/* ------------------------------------------------------------------ */
/* Jobs: PodcastIndex, asked on a thread                                */
/* ------------------------------------------------------------------ */

static void job_free(job_t *job)
{
    if (!job) return;
    free(job->path);
    free(job->items);
    free(job);
}

static void job_task(void *arg)
{
    job_t *job = arg;
    size_t len = 0;
    char error[96] = "";
    char *body = pod_net_api_get(job->path, &len, error, sizeof error);
    int count = -1;
    if (body) {
        size_t size = job->want == WANT_FEEDS      ? sizeof(pod_feed_t)
                    : job->want == WANT_EPISODES   ? sizeof(pod_episode_t)
                                                   : sizeof(pod_category_t);
        job->items = psram_calloc((size_t)job->max, size);
        if (!job->items) {
            pod_text_copy(error, sizeof error, "Out of memory");
        } else if (job->want == WANT_FEEDS) {
            count = pod_api_parse_feeds(body, len, job->items, job->max, error, sizeof error);
        } else if (job->want == WANT_EPISODES) {
            count = pod_api_parse_episodes(body, len, job->items, job->max, error, sizeof error);
        } else {
            count = pod_api_parse_categories(body, len, job->items, job->max, error, sizeof error);
        }
        free(body);
    }
    job->count = count;
    pod_text_copy(job->error, sizeof job->error, error);

    int expected = JOB_RUNNING;
    if (!atomic_compare_exchange_strong(&job->state, &expected, JOB_DONE)) {
        job_free(job);  /* its page went */
    }
#ifndef SIMULATOR_BUILD
    ESP_LOGD(TAG, "job done; %u bytes of stack to spare", (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
    vTaskDelete(NULL);
}

/* Lets go of a page's job: its thread frees it if still running. */
static void abandon_job(page_t *p)
{
    job_t *job = p->job;
    if (!job) return;
    p->job = NULL;
    int expected = JOB_RUNNING;
    if (!atomic_compare_exchange_strong(&job->state, &expected, JOB_ABANDONED)) {
        job_free(job);  /* done already: nobody else will */
    }
}

static void start_job(page_t *p, job_want_t want, const char *path, int max)
{
    abandon_job(p);
    p->loaded = false;
    p->error[0] = '\0';
    job_t *job = psram_calloc(1, sizeof *job);
    if (job) job->path = psram_alloc(strlen(path) + 1);
    if (!job || !job->path) {
        job_free(job);
        p->loaded = true;
        pod_text_copy(p->error, sizeof p->error, "Out of memory");
        return;
    }
    strcpy(job->path, path);
    job->want = want;
    job->max = max;
    atomic_init(&job->state, JOB_RUNNING);
    if (xTaskCreate(job_task, "pod_api", JOB_STACK, job, JOB_PRIORITY, NULL) != pdPASS) {
        job_free(job);
        p->loaded = true;
        pod_text_copy(p->error, sizeof p->error, "Not enough memory to ask PodcastIndex");
        return;
    }
    p->job = job;
}

/* Takes a finished job's answer into its page. True when it had one. */
static bool take_job(page_t *p)
{
    job_t *job = p->job;
    if (!job || atomic_load(&job->state) != JOB_DONE) return false;
    p->job = NULL;
    p->loaded = true;
    static const char *const WHAT[] = {"podcasts", "episodes", "categories"};
    if (job->count < 0) ESP_LOGW(TAG, "no %s: %s", WHAT[job->want], job->error);
    else ESP_LOGI(TAG, "%d %s", job->count, WHAT[job->want]);
    if (job->count < 0) {
        pod_text_copy(p->error, sizeof p->error, job->error[0] ? job->error : "PodcastIndex did not answer");
    } else if (job->want == WANT_FEEDS) {
        free(p->feeds);
        p->feeds = job->items;
        p->feed_count = job->count;
        job->items = NULL;
    } else if (job->want == WANT_EPISODES) {
        free(p->episodes);
        p->episodes = job->items;
        p->episode_count = job->count;
        job->items = NULL;
    } else {
        free(p->categories);
        p->categories = job->items;
        p->category_count = job->count;
        job->items = NULL;
    }
    job_free(job);
    return true;
}

/* ------------------------------------------------------------------ */
/* Pages                                                                */
/* ------------------------------------------------------------------ */

static page_t *page_new(page_kind_t kind, const char *title)
{
    page_t *p = psram_calloc(1, sizeof *p);
    if (!p) return NULL;
    p->kind = kind;
    pod_text_copy(p->title, sizeof p->title, title);
    return p;
}

static void page_free(page_t *p)
{
    if (!p) return;
    abandon_job(p);
    free(p->feeds);
    free(p->episodes);
    free(p->categories);
    free(p->feed);
    free(p->episode);
    free(p);
}

/* The selected row of the page shown, kept for coming back to it. */
static void remember_selection(void)
{
    page_t *p = top();
    if (!p || !s_ui.table) return;
    uint32_t row = 0, col = 0;
    lv_table_get_selected_cell(s_ui.table, &row, &col);
    p->selected = row == LV_TABLE_CELL_NONE ? 0 : (int)row;
    /* Home's rows move as the Now playing row comes and goes: it keeps the
     * item instead. */
    if (p->kind == PAGE_HOME && row < (uint32_t)s_ui.row_count) p->selected = s_ui.rows[row].index;
}

/* The home page's row for `item`, or 0. */
static uint32_t home_row_of(int item)
{
    for (int i = 0; i < s_ui.row_count; i++) {
        if (s_ui.rows[i].index == item) return (uint32_t)i;
    }
    return 0;
}

static void push(page_t *p)
{
    if (!p) {
        toast_warn("Out of memory");
        return;
    }
    if (s_ui.depth == STACK_MAX) {
        /* Deep enough: the oldest page above Home goes. */
        page_free(s_ui.stack[1]);
        memmove(&s_ui.stack[1], &s_ui.stack[2], (STACK_MAX - 2) * sizeof s_ui.stack[0]);
        s_ui.depth--;
    }
    remember_selection();
    s_ui.stack[s_ui.depth++] = p;
    show_top();
}

bool pod_ui_back(void)
{
    if (s_ui.depth <= 1) return false;
    page_t *p = s_ui.stack[--s_ui.depth];
    if (p->kind == PAGE_PLAYER) pod_ui_player_release();
    page_free(p);
    show_top();
    return true;
}

static bool back_handler(void)
{
    return pod_ui_back();
}

static void open_list(list_kind_t list, const char *title, const char *query)
{
    page_t *p = page_new(PAGE_LIST, title);
    if (p) {
        p->list = list;
        pod_text_copy(p->query, sizeof p->query, query ? query : "");
    }
    push(p);
}

static void open_podcast(const pod_feed_t *feed)
{
    page_t *p = page_new(PAGE_PODCAST, "Podcast");
    if (p) {
        p->feed = psram_alloc(sizeof *p->feed);
        if (!p->feed) {
            page_free(p);
            p = NULL;
        } else {
            *p->feed = *feed;
        }
    }
    if (p) {
        pod_lib_view_feed(feed, now());
        pod_lib_flush();
    }
    push(p);
}

static void open_episode(const pod_episode_t *episode)
{
    page_t *p = page_new(PAGE_EPISODE, "Episode");
    if (p) {
        p->episode = psram_alloc(sizeof *p->episode);
        if (!p->episode) {
            page_free(p);
            p = NULL;
        } else {
            *p->episode = *episode;
        }
    }
    if (p) {
        pod_lib_view_episode(episode, now());
        pod_lib_flush();
    }
    push(p);
}

void pod_ui_open_player(void)
{
    /* One Now playing page: going to it again goes back to it. */
    for (int i = s_ui.depth - 1; i > 0; i--) {
        if (s_ui.stack[i]->kind == PAGE_PLAYER) {
            while (s_ui.depth - 1 > i) page_free(s_ui.stack[--s_ui.depth]);
            show_top();
            return;
        }
    }
    push(page_new(PAGE_PLAYER, "Now playing"));
}

/* ------------------------------------------------------------------ */
/* Lists                                                                */
/* ------------------------------------------------------------------ */

static bool selectable(uint32_t row)
{
    if (row >= (uint32_t)s_ui.row_count) return false;
    uint8_t kind = s_ui.rows[row].kind;
    return kind != ROW_NOTE && kind != ROW_HEADING && kind != ROW_ABOUT;
}

/* Selects `row`, or the first that can be chosen at or after it. */
static void select_row(uint32_t row)
{
    if (!s_ui.table || s_ui.row_count == 0) return;
    uint32_t r = row;
    while (r < (uint32_t)s_ui.row_count && !selectable(r)) r++;
    if (r >= (uint32_t)s_ui.row_count) {
        r = row < (uint32_t)s_ui.row_count ? row : 0;
        while (r > 0 && !selectable(r)) r--;
    }
    s_ui.filling = true;
    lv_table_set_selected_cell(s_ui.table, (uint16_t)r, 0);
    s_ui.filling = false;
}

static void list_changed_cb(lv_event_t *e)
{
    if (s_ui.filling) return;
    /* The selection also moves with the arrow keys, which must not open it. */
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD &&
        lv_indev_get_key(indev) != LV_KEY_ENTER) {
        return;
    }
    uint32_t row = 0, col = 0;
    lv_table_get_selected_cell(lv_event_get_target(e), &row, &col);
    if (row != LV_TABLE_CELL_NONE) open_row(row);
}

static void list_key_cb(lv_event_t *e)
{
    lv_obj_t *table = lv_event_get_target(e);
    uint32_t key = lv_event_get_key(e);
    int step = key == 'j' || key == LV_KEY_DOWN ? 1 : key == 'k' || key == LV_KEY_UP ? -1 : 0;
    if (!step) {
        pod_ui_common_key(key);
        return;
    }
    uint32_t row = 0, col = 0;
    lv_table_get_selected_cell(table, &row, &col);
    uint32_t rows = lv_table_get_row_count(table);
    if (row == LV_TABLE_CELL_NONE) row = 0;
    /* The table has moved the arrows' selection already, not j's and k's. */
    else if (key == 'j' && row + 1 < rows) row++;
    else if (key == 'k' && row > 0) row--;
    uint32_t from = row;
    while (!selectable(row) && (int)row + step >= 0 && (int)row + step < (int)rows) {
        row = (uint32_t)((int)row + step);
    }
    if (!selectable(row)) {
        /* Nothing that way. Up from the first row of a search's results
         * goes back to its query. */
        if (step < 0 && s_ui.query_ta) {
            pod_ui_focus(s_ui.query_ta);
            return;
        }
        row = from;
        while (!selectable(row) && row + 1 < rows) row++;
    }
    s_ui.filling = true;
    lv_table_set_selected_cell(table, (uint16_t)row, 0);
    s_ui.filling = false;
    /* What is above the first row that can be chosen is shown with it. */
    bool first = true;
    for (uint32_t r = 0; r < row; r++) first = first && !selectable(r);
    if (first) lv_obj_scroll_to_y(table, 0, LV_ANIM_OFF);
}

/* Notes, headings and the about row are drawn plainer than the rest, and
 * never selected. */
static void list_draw_cb(lv_event_t *e)
{
    lv_draw_task_t *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(task);
    if (base->part != LV_PART_ITEMS || base->id1 >= (uint32_t)s_ui.row_count) return;
    uint8_t kind = s_ui.rows[base->id1].kind;
    if (kind != ROW_NOTE && kind != ROW_HEADING && kind != ROW_ABOUT) return;

    const theme_colors_t *clr = theme_get_colors();
    if (lv_draw_task_get_type(task) == LV_DRAW_TASK_TYPE_FILL) {
        lv_draw_task_get_fill_dsc(task)->color = clr->bg;
    } else if (lv_draw_task_get_type(task) == LV_DRAW_TASK_TYPE_LABEL) {
        lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(task);
        label->color = kind == ROW_ABOUT ? clr->text : clr->text_secondary;
    }
}

static lv_obj_t *make_table(lv_obj_t *page, int32_t y)
{
    const theme_colors_t *clr = theme_get_colors();
    lv_obj_t *table = lv_table_create(page);
    lv_obj_set_pos(table, 0, y);
    lv_obj_set_size(table, s_ui.w, s_ui.h - y);
    lv_obj_set_style_bg_color(table, clr->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(table, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(table, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(table, &s_text_font, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, clr->text, LV_PART_ITEMS);
    lv_obj_set_style_text_line_space(table, LINE_SPACE, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(table, clr->surface, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_ITEMS);
    theme_rule_table_rows(table);
    lv_obj_set_style_pad_left(table, CELL_PAD_W, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(table, CELL_PAD_W, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 4, LV_PART_ITEMS);
    /* The selected row, inverted, as the other lists draw a pressed one. */
    lv_obj_set_style_bg_color(table, clr->primary, LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(table, clr->bg, LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(table, clr->primary, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(table, clr->bg, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_scrollbar_mode(table, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(table, clr->primary, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(table, 2, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(table, list_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(table, list_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(table, list_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_flag(table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_table_set_column_count(table, 1);
    lv_table_set_column_width(table, 0, s_ui.w);
    s_ui.table = table;
    return table;
}

/* Appends `text` to `cell` in at most `max_lines` lines of a row, the last
 * `reserve` px short: cut with an ellipsis if it does not fit. As the
 * Reader's library, with the lines measured as the table will break them. */
static void append_fitted(char *cell, size_t cap, const char *text, int max_lines, int32_t reserve)
{
    size_t len = strlen(text);
    char *s = arabic_present(text, len) ? arabic_shape(text, len, NULL, 0, &len) : NULL;
    if (s) text = s;

    const lv_font_t *font = &s_text_font;
    int32_t width = s_ui.w - 2 * CELL_PAD_W;
    size_t pos = 0, line_start = 0;
    for (int line = 0; pos < len && line < max_lines; line++) {
        line_start = pos;
        int32_t room = line == max_lines - 1 ? width - reserve : width;
        uint32_t n = lv_text_get_next_line(text + pos, font, 0, room, NULL, LV_TEXT_FLAG_NONE);
        pos += n ? n : 1;
    }

    size_t end = pos;
    bool cut = pos < len;
    if (cut) {
        int32_t room = width - reserve - lv_text_get_width(ELLIPSIS, sizeof(ELLIPSIS) - 1, font, 0);
        while (end > line_start &&
               lv_text_get_width(text + line_start, (uint32_t)(end - line_start), font, 0) > room) {
            do end--; while (end > line_start && ((unsigned char)text[end] & 0xc0) == 0x80);
        }
    }
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\n')) end--;

    size_t used = strlen(cell);
    if (used + end + sizeof(ELLIPSIS) < cap) {
        memcpy(cell + used, text, end);
        strcpy(cell + used + end, cut ? ELLIPSIS : "");
    }
    free(s);
}

static void cat(char *cell, size_t cap, const char *text)
{
    size_t used = strlen(cell);
    if (used + 1 < cap) pod_text_copy(cell + used, cap - used, text);
}

static void add_row(row_kind_t kind, int index, const char *text)
{
    if (s_ui.row_count >= MAX_ROWS) return;
    uint32_t row = (uint32_t)s_ui.row_count;
    if (lv_table_get_row_count(s_ui.table) <= row) lv_table_set_row_count(s_ui.table, row + 1);
    s_ui.rows[row].kind = (uint8_t)kind;
    s_ui.rows[row].index = (int16_t)index;
    s_ui.row_count++;
    if (kind == ROW_NOTE || kind == ROW_HEADING) {
        char *s = shaped(text);
        lv_table_set_cell_value(s_ui.table, row, 0, s ? s : text);
        free(s);
    } else {
        /* The others are fitted, and shaped, by whoever made them. */
        lv_table_set_cell_value(s_ui.table, row, 0, text);
    }
}

/* A podcast's row: its title in two lines at most, a star before it if
 * starred, then who makes it and how many episodes it has. */
static void feed_cell(const pod_feed_t *f, char *cell, size_t cap)
{
    cell[0] = '\0';
    if (pod_lib_feed_starred(f->id)) cat(cell, cap, STAR " ");
    append_fitted(cell, cap, f->title[0] ? f->title : "Untitled", 2, 0);
    cat(cell, cap, "\n");

    char more[48] = "";
    if (f->episode_count > 0) {
        snprintf(more, sizeof more, "%d episode%s", (int)f->episode_count, f->episode_count == 1 ? "" : "s");
    }
    if (f->author[0] && more[0]) {
        char after[56];
        snprintf(after, sizeof after, DOT "%s", more);
        append_fitted(cell, cap, f->author, 1, lv_text_get_width(after, (uint32_t)strlen(after), &s_text_font, 0));
        cat(cell, cap, after);
    } else {
        append_fitted(cell, cap, f->author[0] ? f->author : more[0] ? more : f->categories, 1, 0);
    }
}

/* What a row says of an episode's state: starred, saved, how far played. */
static void episode_marks(const pod_episode_t *e, char *out, size_t cap)
{
    out[0] = '\0';
    if (!pod_episode_playable(e)) {
        cat(out, cap, "not MP3");
        return;
    }
    pod_lib_episode(e->id, &s_known);
    pod_player_status_t st;
    pod_player_status(&st);
    if (st.episode_id == e->id && (st.state == POD_PLAYER_PLAYING || st.state == POD_PLAYER_LOADING)) {
        cat(out, cap, LV_SYMBOL_PLAY " ");
    }
    if (s_known.starred_at) cat(out, cap, STAR " ");
    /* The library's word, not the card's: a stat a row would hold up the
     * list on the SD card's bus. */
    if (s_known.saved_at) cat(out, cap, LV_SYMBOL_SD_CARD " ");
    if (s_known.finished) {
        cat(out, cap, LV_SYMBOL_OK " ");
    } else if (s_known.position > 0 && e->duration > 0) {
        char left[24];
        pod_text_duration(e->duration - s_known.position, left, sizeof left);
        if (left[0]) {
            cat(out, cap, left);
            cat(out, cap, " left ");
        }
    }
}

/* An episode's row: its title in two lines at most, then its podcast (when
 * the list is of more than one), its date and length, and its marks. */
static void episode_cell(const pod_episode_t *e, bool with_feed, char *cell, size_t cap)
{
    cell[0] = '\0';
    append_fitted(cell, cap, e->title[0] ? e->title : "Untitled", 2, 0);
    cat(cell, cap, "\n");

    char date[24], length[24], marks[64], tail[128];
    pod_text_date(e->published, now(), date, sizeof date);
    pod_text_duration(e->duration, length, sizeof length);
    episode_marks(e, marks, sizeof marks);
    snprintf(tail, sizeof tail, "%s%s%s%s%s", date, date[0] && length[0] ? DOT : "", length,
             marks[0] && (date[0] || length[0]) ? DOT : "", marks);

    if (with_feed) {
        const char *feed = e->feed_title;
        pod_lib_feed_t lf;
        if (!feed[0] && pod_lib_feed(e->feed_id, &lf)) feed = lf.feed.title;
        if (feed[0]) {
            char after[136];
            snprintf(after, sizeof after, DOT "%s", tail);
            append_fitted(cell, cap, feed, 1,
                          tail[0] ? lv_text_get_width(after, (uint32_t)strlen(after), &s_text_font, 0) : 0);
            if (tail[0]) cat(cell, cap, after);
            return;
        }
    }
    append_fitted(cell, cap, tail, 1, 0);
}

static void add_feed_rows(const pod_feed_t *feeds, int count)
{
    char *cell = psram_alloc(CELL_MAX);
    if (!cell) return;
    for (int i = 0; i < count; i++) {
        feed_cell(&feeds[i], cell, CELL_MAX);
        add_row(ROW_FEED, i, cell);
    }
    free(cell);
}

static void add_episode_rows(const pod_episode_t *episodes, int count, bool with_feed)
{
    char *cell = psram_alloc(CELL_MAX);
    if (!cell) return;
    for (int i = 0; i < count; i++) {
        episode_cell(&episodes[i], with_feed, cell, CELL_MAX);
        add_row(ROW_EPISODE, i, cell);
    }
    free(cell);
}

/* Takes the library's podcasts and episodes on `which` list into the page. */
static void read_library(page_t *p, pod_lib_list_t which, bool feeds)
{
    free(p->feeds);
    free(p->episodes);
    p->feeds = NULL;
    p->episodes = NULL;
    p->feed_count = p->episode_count = 0;
    if (feeds) {
        int n = pod_lib_feeds(which, s_feed_list, SECTION_MAX);
        p->feeds = n ? psram_alloc((size_t)n * sizeof *p->feeds) : NULL;
        if (p->feeds) {
            memcpy(p->feeds, s_feed_list, (size_t)n * sizeof *p->feeds);
            p->feed_count = n;
        }
    }
    int n = pod_lib_episodes(which, s_lib_list, SECTION_MAX);
    p->episodes = n ? psram_alloc((size_t)n * sizeof *p->episodes) : NULL;
    if (p->episodes) {
        for (int i = 0; i < n; i++) p->episodes[i] = s_lib_list[i].episode;
        p->episode_count = n;
    }
    p->loaded = true;
}

/* The rows of a list or a podcast's page, from what the page has. */
static void fill_rows(page_t *p)
{
    if (!s_ui.table) return;
    lv_obj_t *table = s_ui.table;
    s_ui.row_count = 0;
    lv_table_set_row_count(table, 1);

    if (p->kind == PAGE_HOME) {
        /* The home page's rows are its own; see fill_home(). */
        return;
    }

    if (p->kind == PAGE_PODCAST) {
        const pod_feed_t *f = p->feed;
        char *cell = psram_alloc(CELL_MAX);
        if (cell) {
            cell[0] = '\0';
            /* Short: the episodes are what the page is for. */
            append_fitted(cell, CELL_MAX, f->title[0] ? f->title : "Untitled", 2, 0);
            char meta[160] = "";
            if (f->author[0]) cat(meta, sizeof meta, f->author);
            if (f->episode_count > 0) {
                char n[32];
                snprintf(n, sizeof n, "%s%d episodes", meta[0] ? DOT : "", (int)f->episode_count);
                cat(meta, sizeof meta, n);
            }
            if (f->categories[0]) {
                cat(meta, sizeof meta, meta[0] ? DOT : "");
                cat(meta, sizeof meta, f->categories);
            }
            if (meta[0]) {
                cat(cell, CELL_MAX, "\n");
                append_fitted(cell, CELL_MAX, meta, 1, 0);
            }
            if (f->description[0]) {
                cat(cell, CELL_MAX, "\n");
                append_fitted(cell, CELL_MAX, f->description, 2, 0);
            }
            add_row(ROW_ABOUT, 0, cell);
            free(cell);
        }
        if (!p->loaded) {
            add_row(ROW_NOTE, 0, "Loading episodes" ELLIPSIS);
        } else {
            if (p->error[0]) {
                char note[160];
                snprintf(note, sizeof note, "%s%s", p->error,
                         p->episode_count ? "\nThe episodes known here:" : "");
                add_row(ROW_NOTE, 0, note);
                add_row(ROW_RETRY, 0, LV_SYMBOL_REFRESH " Try again");
            } else if (!p->episode_count) {
                add_row(ROW_NOTE, 0, "No episodes.");
            }
            add_episode_rows(p->episodes, p->episode_count, false);
        }
        lv_table_set_row_count(table, (uint32_t)(s_ui.row_count ? s_ui.row_count : 1));
        return;
    }

    if (p->kind == PAGE_SEARCH && !p->query[0]) {
        add_row(ROW_NOTE, 0, "Type a name and press Enter.");
        return;
    }
    if (!p->loaded) {
        add_row(ROW_NOTE, 0, p->kind == PAGE_SEARCH ? "Searching" ELLIPSIS : "Loading" ELLIPSIS);
        return;
    }
    if (p->error[0]) {
        add_row(ROW_NOTE, 0, p->error);
        add_row(ROW_RETRY, 0, LV_SYMBOL_REFRESH " Try again");
        return;
    }

    if (p->kind == PAGE_SEARCH) {
        if (pod_search_finds_episodes(p->mode)) add_episode_rows(p->episodes, p->episode_count, true);
        else add_feed_rows(p->feeds, p->feed_count);
        if (!s_ui.row_count) add_row(ROW_NOTE, 0, "Nothing found.");
    } else {
        switch (p->list) {
        case LIST_CATEGORIES:
            for (int i = 0; i < p->category_count; i++) add_row(ROW_CATEGORY, i, p->categories[i].name);
            break;
        case LIST_STARRED:
        case LIST_VIEWED:
            if (p->feed_count) {
                add_row(ROW_HEADING, 0, "Podcasts");
                add_feed_rows(p->feeds, p->feed_count);
            }
            if (p->episode_count) {
                add_row(ROW_HEADING, 0, "Episodes");
                add_episode_rows(p->episodes, p->episode_count, true);
            }
            break;
        case LIST_NEW:
        case LIST_SAVED:
        case LIST_CONTINUE:
            add_episode_rows(p->episodes, p->episode_count, true);
            break;
        default:
            add_feed_rows(p->feeds, p->feed_count);
            break;
        }
        if (!s_ui.row_count) {
            const char *empty =
                p->list == LIST_STARRED  ? "Nothing starred yet. Star a podcast or an episode on its page."
              : p->list == LIST_SAVED    ? "Nothing saved yet. Save an episode from its page to play it with no network."
              : p->list == LIST_VIEWED   ? "Nothing opened yet."
              : p->list == LIST_CONTINUE ? "Nothing begun yet."
              : p->list == LIST_NEW      ? "Star podcasts to see their newest episodes here."
                                         : "Nothing here.";
            add_row(ROW_NOTE, 0, empty);
        }
    }
    lv_table_set_row_count(table, (uint32_t)(s_ui.row_count ? s_ui.row_count : 1));
}

/* Fetches or reads what a list page shows. */
static void start_load(page_t *p)
{
    char path[2304];
    if (p->kind == PAGE_PODCAST) {
        p->offline = false;
        if (pod_api_episodes_path(path, sizeof path, &p->feed->id, 1, RESULTS_MAX)) {
            start_job(p, WANT_EPISODES, path, RESULTS_MAX);
        }
        return;
    }
    if (p->kind == PAGE_SEARCH) {
        if (!p->query[0]) return;
        if (!pod_api_search_path(path, sizeof path, p->mode, p->query, p->clean, 40)) {
            p->loaded = true;
            pod_text_copy(p->error, sizeof p->error, "That is too long to search for");
            return;
        }
        start_job(p, pod_search_finds_episodes(p->mode) ? WANT_EPISODES : WANT_FEEDS, path, 40);
        return;
    }
    switch (p->list) {
    case LIST_TRENDING:
    case LIST_CATEGORY:
        if (pod_api_trending_path(path, sizeof path, p->list == LIST_CATEGORY ? p->query : "", "", RESULTS_MAX)) {
            start_job(p, WANT_FEEDS, path, RESULTS_MAX);
        }
        break;
    case LIST_CATEGORIES:
        if (pod_api_categories_path(path, sizeof path)) start_job(p, WANT_CATEGORIES, path, 200);
        break;
    case LIST_NEW: {
        int64_t ids[100];
        int n = pod_lib_starred_feed_ids(ids, 100);
        if (!n) {
            p->loaded = true;
        } else if (pod_api_episodes_path(path, sizeof path, ids, n, RESULTS_MAX)) {
            start_job(p, WANT_EPISODES, path, RESULTS_MAX);
        }
        break;
    }
    case LIST_STARRED:  read_library(p, POD_LIB_STARRED, true); break;
    case LIST_VIEWED:   read_library(p, POD_LIB_VIEWED, true); break;
    case LIST_SAVED:    read_library(p, POD_LIB_SAVED, false); break;
    case LIST_CONTINUE: read_library(p, POD_LIB_CONTINUE, false); break;
    }
}

/* A podcast page whose episodes could not be fetched shows the ones the
 * library knows: saved ones can be played with no network. */
static void podcast_offline(page_t *p)
{
    int n = pod_lib_feed_episodes(p->feed->id, s_lib_list, SECTION_MAX);
    free(p->episodes);
    p->episodes = n ? psram_alloc((size_t)n * sizeof *p->episodes) : NULL;
    p->episode_count = 0;
    if (p->episodes) {
        for (int i = 0; i < n; i++) p->episodes[i] = s_lib_list[i].episode;
        p->episode_count = n;
    }
    p->offline = true;
}

static void build_list_page(page_t *p)
{
    lv_obj_t *page = pod_ui_page(p->title, NULL);
    s_ui.screen = page;
    make_table(page, POD_UI_TITLE_H);
    if (!p->loaded && !p->job) start_load(p);
    fill_rows(p);
    select_row((uint32_t)p->selected);
    pod_ui_focus(s_ui.table);
}

/* ------------------------------------------------------------------ */
/* Home                                                                 */
/* ------------------------------------------------------------------ */

static void home_row(home_item_t item, const char *text)
{
    add_row(ROW_HOME, item, text);
}

static void counted(char *out, size_t cap, const char *label, int n)
{
    if (n > 0) snprintf(out, cap, "%s (%d)", label, n);
    else snprintf(out, cap, "%s", label);
}

static void fill_home(void)
{
    char text[CELL_MAX / 2];
    s_ui.row_count = 0;
    lv_table_set_row_count(s_ui.table, 1);

    pod_player_status_t st;
    pod_player_status(&st);
    s_ui.home_state = st.state;
    s_ui.home_episode = st.episode_id;
    pod_episode_t *playing = psram_alloc(sizeof *playing);
    if (playing && pod_player_episode(playing)) {
        bool on = st.state == POD_PLAYER_PLAYING || st.state == POD_PLAYER_LOADING;
        snprintf(text, sizeof text, "%s %s\n", on ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE,
                 st.state == POD_PLAYER_ENDED ? "Played" : on ? "Now playing" : "Paused");
        append_fitted(text, sizeof text, playing->title, 1, 0);
        home_row(HOME_PLAYING, text);
    }
    free(playing);

    home_row(HOME_SEARCH, LV_SYMBOL_KEYBOARD "  Search");
    home_row(HOME_TRENDING, LV_SYMBOL_UP "  Trending");
    home_row(HOME_CATEGORIES, LV_SYMBOL_LIST "  Categories");
    home_row(HOME_NEW, LV_SYMBOL_BELL "  New episodes");
    char label[64];
    counted(label, sizeof label, STAR "  Starred",
            pod_lib_count_feeds(POD_LIB_STARRED) + pod_lib_count_episodes(POD_LIB_STARRED));
    home_row(HOME_STARRED, label);
    counted(label, sizeof label, LV_SYMBOL_SD_CARD "  Saved", pod_lib_count_episodes(POD_LIB_SAVED));
    home_row(HOME_SAVED, label);
    counted(label, sizeof label, LV_SYMBOL_PLAY "  Continue listening",
            pod_lib_count_episodes(POD_LIB_CONTINUE));
    home_row(HOME_CONTINUE, label);
    home_row(HOME_VIEWED, LV_SYMBOL_EYE_OPEN "  Recently viewed");
    home_row(HOME_SETUP, pod_cred_on_card() || pod_cred_built_in()
                             ? LV_SYMBOL_SETTINGS "  PodcastIndex key"
                             : LV_SYMBOL_WARNING "  Set up a PodcastIndex key");
    lv_table_set_row_count(s_ui.table, (uint32_t)s_ui.row_count);
}

static void build_home(page_t *p)
{
    lv_obj_t *page = pod_ui_page("Podcasts", NULL);
    s_ui.screen = page;
    make_table(page, POD_UI_TITLE_H);
    fill_home();
    select_row(p->selected ? home_row_of(p->selected) : 0);
    pod_ui_focus(s_ui.table);
}

static void open_home_item(home_item_t item)
{
    switch (item) {
    case HOME_PLAYING:    pod_ui_open_player(); break;
    case HOME_SEARCH:     push(page_new(PAGE_SEARCH, "Search")); break;
    case HOME_TRENDING:   open_list(LIST_TRENDING, "Trending", NULL); break;
    case HOME_CATEGORIES: open_list(LIST_CATEGORIES, "Categories", NULL); break;
    case HOME_NEW:        open_list(LIST_NEW, "New episodes", NULL); break;
    case HOME_STARRED:    open_list(LIST_STARRED, "Starred", NULL); break;
    case HOME_SAVED:      open_list(LIST_SAVED, "Saved", NULL); break;
    case HOME_VIEWED:     open_list(LIST_VIEWED, "Recently viewed", NULL); break;
    case HOME_CONTINUE:   open_list(LIST_CONTINUE, "Continue listening", NULL); break;
    case HOME_SETUP:      push(page_new(PAGE_SETUP, "PodcastIndex key")); break;
    }
}

static void open_row(uint32_t row)
{
    page_t *p = top();
    if (!p || row >= (uint32_t)s_ui.row_count) return;
    int index = s_ui.rows[row].index;
    switch (s_ui.rows[row].kind) {
    case ROW_HOME:
        open_home_item((home_item_t)index);
        break;
    case ROW_FEED:
        if (index < p->feed_count) open_podcast(&p->feeds[index]);
        break;
    case ROW_EPISODE:
        if (index < p->episode_count) {
            pod_episode_t *e = &p->episodes[index];
            /* What it is from, when the list did not say. */
            if (!e->feed_title[0] && p->kind == PAGE_PODCAST) {
                pod_text_copy(e->feed_title, sizeof e->feed_title, p->feed->title);
            }
            open_episode(e);
        }
        break;
    case ROW_CATEGORY:
        if (index < p->category_count) {
            open_list(LIST_CATEGORY, p->categories[index].name, p->categories[index].name);
        }
        break;
    case ROW_RETRY:
        p->error[0] = '\0';
        p->loaded = false;
        start_load(p);
        fill_rows(p);
        select_row(0);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Search                                                               */
/* ------------------------------------------------------------------ */

static void run_search(page_t *p)
{
    const char *text = lv_textarea_get_text(s_ui.query_ta);
    while (*text == ' ') text++;
    pod_text_copy(p->query, sizeof p->query, text);
    size_t n = strlen(p->query);
    while (n && p->query[n - 1] == ' ') p->query[--n] = '\0';
    free(p->feeds);
    free(p->episodes);
    p->feeds = NULL;
    p->episodes = NULL;
    p->feed_count = p->episode_count = 0;
    p->selected = 0;
    p->loaded = false;
    p->error[0] = '\0';
    abandon_job(p);
    start_load(p);
    fill_rows(p);
}

static void search_options_text(page_t *p)
{
    char text[48];
    snprintf(text, sizeof text, "%s " LV_SYMBOL_DOWN, pod_search_mode_name(p->mode));
    pod_ui_set_text(pod_ui_button_label(s_ui.mode_btn), text);
    pod_ui_set_text(pod_ui_button_label(s_ui.clean_btn), p->clean ? "Clean only" : "Explicit too");
}

static void query_cb(lv_event_t *e)
{
    page_t *p = top();
    if (!p || p->kind != PAGE_SEARCH) return;
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        run_search(p);
        return;
    }
    /* Down from the query goes to its results. */
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_DOWN && s_ui.row_count) {
        pod_ui_focus(s_ui.table);
        select_row(0);
    }
}

static void mode_cb(lv_event_t *e)
{
    (void)e;
    page_t *p = top();
    if (!p || p->kind != PAGE_SEARCH) return;
    p->mode = (pod_search_mode_t)((p->mode + 1) % POD_SEARCH_MODES);
    search_options_text(p);
    if (p->query[0]) run_search(p);
}

static void clean_cb(lv_event_t *e)
{
    (void)e;
    page_t *p = top();
    if (!p || p->kind != PAGE_SEARCH) return;
    p->clean = !p->clean;
    search_options_text(p);
    if (p->query[0]) run_search(p);
}

static void build_search(page_t *p)
{
    const theme_colors_t *clr = theme_get_colors();
    lv_obj_t *page = pod_ui_page(p->title, NULL);
    s_ui.screen = page;
    int32_t y = POD_UI_TITLE_H + 4;

    lv_obj_t *ta = lv_textarea_create(page);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, sizeof p->query - 1);
    lv_textarea_set_placeholder_text(ta, "Name, title or person");
    lv_obj_set_pos(ta, POD_UI_PAD / 2, y);
    lv_obj_set_width(ta, s_ui.w - POD_UI_PAD);
    lv_obj_set_style_text_font(ta, &s_text_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, clr->text, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, clr->bg, LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, clr->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(ta, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(ta, clr->radius, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(ta, 4, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, clr->text_secondary, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_textarea_set_text(ta, p->query);
    lv_obj_add_event_cb(ta, query_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ta, query_cb, LV_EVENT_KEY, NULL);
    s_ui.query_ta = ta;
    lv_obj_update_layout(ta);
    y += lv_obj_get_height(ta) + 4;

    int32_t half = (s_ui.w - POD_UI_PAD * 3 / 2) / 2;
    s_ui.mode_btn = pod_ui_button(page, "", NULL, mode_cb, NULL);
    lv_obj_set_pos(s_ui.mode_btn, POD_UI_PAD / 2, y);
    lv_obj_set_width(s_ui.mode_btn, half);
    s_ui.clean_btn = pod_ui_button(page, "", NULL, clean_cb, NULL);
    lv_obj_set_pos(s_ui.clean_btn, POD_UI_PAD + half, y);
    lv_obj_set_width(s_ui.clean_btn, half);
    search_options_text(p);
    lv_obj_update_layout(s_ui.mode_btn);
    y += lv_obj_get_height(s_ui.mode_btn) + 4;

    lv_group_t *grp = lv_group_get_default();
    if (grp) {
        lv_group_add_obj(grp, s_ui.mode_btn);
        lv_group_add_obj(grp, s_ui.clean_btn);
    }
    make_table(page, y);
    fill_rows(p);
    if (p->query[0] && p->loaded && !p->error[0]) {
        select_row((uint32_t)p->selected);
        pod_ui_focus(s_ui.table);
    } else {
        pod_ui_focus(ta);
    }
}

/* ------------------------------------------------------------------ */
/* A podcast                                                            */
/* ------------------------------------------------------------------ */

static void refresh_star(bool starred)
{
    if (s_ui.star) pod_ui_set_text(pod_ui_button_label(s_ui.star), starred ? STAR : STAR_HOLLOW);
}

static void podcast_star_cb(lv_event_t *e)
{
    (void)e;
    page_t *p = top();
    if (!p || p->kind != PAGE_PODCAST) return;
    bool starred = !pod_lib_feed_starred(p->feed->id);
    pod_lib_star_feed(p->feed, starred, now());
    pod_lib_flush();
    refresh_star(starred);
    toast_show(starred ? "Starred" : "No longer starred", TOAST_INFO, 1500);
}

static void build_podcast(page_t *p)
{
    lv_obj_t *page = pod_ui_page(p->feed->title[0] ? p->feed->title : "Podcast", &s_ui.star);
    s_ui.screen = page;
    lv_obj_add_event_cb(s_ui.star, podcast_star_cb, LV_EVENT_CLICKED, NULL);
    refresh_star(pod_lib_feed_starred(p->feed->id));
    make_table(page, POD_UI_TITLE_H);
    if (!p->loaded && !p->job) start_load(p);
    fill_rows(p);
    select_row((uint32_t)p->selected);
    pod_ui_focus(s_ui.table);
}

/* ------------------------------------------------------------------ */
/* An episode                                                           */
/* ------------------------------------------------------------------ */

static void episode_buttons(page_t *p)
{
    const pod_episode_t *e = p->episode;
    char text[64];

    /* Play: what it would do. */
    pod_player_status_t st;
    pod_player_status(&st);
    pod_lib_episode(e->id, &s_known);
    if (!pod_episode_playable(e)) {
        snprintf(text, sizeof text, "Not playable");
        lv_obj_add_state(s_ui.play_btn, LV_STATE_DISABLED);
    } else if (st.episode_id == e->id && (st.state == POD_PLAYER_PLAYING || st.state == POD_PLAYER_LOADING)) {
        snprintf(text, sizeof text, LV_SYMBOL_AUDIO " Playing");
    } else if (st.episode_id == e->id && st.state == POD_PLAYER_PAUSED) {
        char at[16];
        pod_text_clock(st.position, at, sizeof at);
        snprintf(text, sizeof text, LV_SYMBOL_PLAY " Resume %s", at);
    } else if (s_known.position > 0 && !s_known.finished) {
        char at[16];
        pod_text_clock(s_known.position, at, sizeof at);
        snprintf(text, sizeof text, LV_SYMBOL_PLAY " Resume %s", at);
    } else {
        snprintf(text, sizeof text, LV_SYMBOL_PLAY " Play");
    }
    pod_ui_set_text(pod_ui_button_label(s_ui.play_btn), text);

    /* Save: where the file is. */
    pod_dl_status_t dl;
    pod_download_status(e, &dl);
    char status[96] = "";
    switch (dl.state) {
    case POD_DL_QUEUED:
        snprintf(text, sizeof text, LV_SYMBOL_CLOSE " Waiting");
        snprintf(status, sizeof status, "Waiting to be saved. Tap to cancel.");
        break;
    case POD_DL_RUNNING: {
        int pct = dl.total > 0 ? (int)(dl.done * 100 / dl.total) : 0;
        char done[16], total[16];
        pod_text_size(dl.done, done, sizeof done);
        pod_text_size(dl.total, total, sizeof total);
        snprintf(text, sizeof text, LV_SYMBOL_CLOSE " %d%%", pct);
        if (dl.total > 0) snprintf(status, sizeof status, "Saving: %s of %s. Tap to cancel.", done, total);
        else snprintf(status, sizeof status, "Saving: %s. Tap to cancel.", done);
        break;
    }
    case POD_DL_FAILED:
        snprintf(text, sizeof text, LV_SYMBOL_DOWNLOAD " Retry");
        snprintf(status, sizeof status, "Not saved: %s", dl.error);
        break;
    case POD_DL_SAVED:
        if (s_ui.confirm_ticks > 0) {
            snprintf(text, sizeof text, LV_SYMBOL_TRASH " Delete?");
            snprintf(status, sizeof status, "Tap again to delete it from the SD card.");
        } else {
            snprintf(text, sizeof text, LV_SYMBOL_SD_CARD " Saved");
            snprintf(status, sizeof status, "On the SD card, to play with no network.");
        }
        break;
    default:
        snprintf(text, sizeof text, LV_SYMBOL_DOWNLOAD " Save");
        if (!pod_episode_playable(e)) lv_obj_add_state(s_ui.save_btn, LV_STATE_DISABLED);
        break;
    }
    if (!pod_episode_playable(e)) {
        snprintf(status, sizeof status, "Only MP3 episodes play here; this one is %s.",
                 e->type[0] ? e->type : "of an unknown type");
    }
    pod_ui_set_text(pod_ui_button_label(s_ui.save_btn), text);
    pod_ui_set_text(s_ui.status_lbl, status);
    if (status[0]) lv_obj_clear_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void play_cb(lv_event_t *e)
{
    (void)e;
    page_t *p = top();
    if (!p || p->kind != PAGE_EPISODE) return;
    pod_player_status_t st;
    pod_player_status(&st);
    if (st.episode_id == p->episode->id && st.state != POD_PLAYER_ENDED && st.state != POD_PLAYER_FAILED) {
        if (st.state == POD_PLAYER_PAUSED) pod_player_resume();
    } else {
        const char *why = pod_player_play(p->episode, -1);
        if (why) {
            toast_warn(why);
            return;
        }
    }
    pod_ui_open_player();
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    page_t *p = top();
    if (!p || p->kind != PAGE_EPISODE) return;
    pod_dl_status_t dl;
    pod_download_status(p->episode, &dl);
    if (dl.state == POD_DL_QUEUED || dl.state == POD_DL_RUNNING) {
        pod_download_cancel(p->episode->id);
        toast_show("Not saved", TOAST_INFO, 1500);
    } else if (dl.state == POD_DL_SAVED) {
        if (s_ui.confirm_ticks > 0) {
            s_ui.confirm_ticks = 0;
            pod_player_status_t st;
            pod_player_status(&st);
            if (st.episode_id == p->episode->id && st.local) pod_player_stop();
            if (pod_download_delete(p->episode)) toast_show("Deleted from the SD card", TOAST_INFO, 2000);
            else toast_warn("Could not delete it");
        } else {
            /* A second tap within 3 s deletes it. */
            s_ui.confirm_ticks = 3000 / TICK_MS;
        }
    } else {
        const char *why = pod_download_start(p->episode);
        if (why) toast_warn(why);
        else toast_show("Saving to the SD card", TOAST_INFO, 1500);
    }
    episode_buttons(p);
}

static void episode_star_cb(lv_event_t *e)
{
    (void)e;
    page_t *p = top();
    if (!p || p->kind != PAGE_EPISODE) return;
    pod_lib_episode(p->episode->id, &s_known);
    bool starred = !s_known.starred_at;
    pod_lib_star_episode(p->episode, starred, now());
    pod_lib_flush();
    refresh_star(starred);
    toast_show(starred ? "Starred" : "No longer starred", TOAST_INFO, 1500);
}

static void episode_key_cb(lv_event_t *e)
{
    pod_ui_common_key(lv_event_get_key(e));
}

static lv_obj_t *text_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "");
    pod_ui_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_font(label, &s_text_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(label, LINE_SPACE, LV_PART_MAIN);
    return label;
}

static void build_episode(page_t *p)
{
    const theme_colors_t *clr = theme_get_colors();
    const pod_episode_t *e = p->episode;
    lv_obj_t *page = pod_ui_page("Episode", &s_ui.star);
    s_ui.screen = page;
    lv_obj_add_event_cb(s_ui.star, episode_star_cb, LV_EVENT_CLICKED, NULL);
    pod_lib_episode(e->id, &s_known);
    refresh_star(s_known.starred_at != 0);

    /* One column that scrolls, for a description of any length. */
    lv_obj_t *col = lv_obj_create(page);
    lv_obj_set_pos(col, 0, POD_UI_TITLE_H);
    lv_obj_set_size(col, s_ui.w, s_ui.h - POD_UI_TITLE_H);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(col, POD_UI_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(col, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(col, clr->primary, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(col, 2, LV_PART_SCROLLBAR);

    text_label(col, e->title[0] ? e->title : "Untitled", clr->text);
    const char *feed = e->feed_title;
    pod_lib_feed_t lf;
    if (!feed[0] && pod_lib_feed(e->feed_id, &lf)) feed = lf.feed.title;
    if (feed[0]) text_label(col, feed, clr->text_secondary);

    char date[24], length[24], size[16], meta[96];
    pod_text_date(e->published, now(), date, sizeof date);
    pod_text_duration(e->duration, length, sizeof length);
    size[0] = '\0';
    if (e->bytes > 0) pod_text_size(e->bytes, size, sizeof size);
    snprintf(meta, sizeof meta, "%s%s%s%s%s", date, date[0] && length[0] ? DOT : "", length,
             size[0] && (date[0] || length[0]) ? DOT : "", size);
    if (meta[0]) text_label(col, meta, clr->text_secondary);

    lv_obj_t *row = lv_obj_create(col);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, POD_UI_PAD, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.play_btn = pod_ui_button(row, "", NULL, play_cb, NULL);
    lv_obj_set_flex_grow(s_ui.play_btn, 1);
    s_ui.save_btn = pod_ui_button(row, "", NULL, save_cb, NULL);
    lv_obj_set_flex_grow(s_ui.save_btn, 1);
    lv_obj_add_event_cb(s_ui.play_btn, episode_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(s_ui.save_btn, episode_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(s_ui.star, episode_key_cb, LV_EVENT_KEY, NULL);

    s_ui.status_lbl = text_label(col, "", clr->text_secondary);
    if (e->description[0]) text_label(col, e->description, clr->text);

    s_ui.confirm_ticks = 0;
    episode_buttons(p);
    lv_group_t *grp = lv_group_get_default();
    if (grp) lv_group_add_obj(grp, s_ui.save_btn);
    if (grp) lv_group_add_obj(grp, s_ui.star);
    pod_ui_focus(s_ui.play_btn);
}

/* ------------------------------------------------------------------ */
/* Setup                                                                */
/* ------------------------------------------------------------------ */

static void setup_save_cb(lv_event_t *e)
{
    (void)e;
    const char *key = lv_textarea_get_text(s_ui.key_ta);
    char secret[POD_CRED_SECRET_MAX], old_key[POD_CRED_KEY_MAX];
    pod_text_copy(secret, sizeof secret, lv_textarea_get_text(s_ui.secret_ta));
    /* The secret left empty keeps the one saved. */
    if (!secret[0] && pod_cred_on_card()) pod_cred_get(old_key, sizeof old_key, secret, sizeof secret);
    bool ok = key[0] && secret[0];
    if (ok && !pod_cred_save(key, secret)) {
        memset(secret, 0, sizeof secret);
        toast_warn("Could not save them to the SD card");
        return;
    }
    memset(secret, 0, sizeof secret);
    if (!ok) {
        toast_warn("Fill in both the key and the secret");
        return;
    }
    lv_textarea_set_text(s_ui.secret_ta, "");
    toast_show("Saved", TOAST_SUCCESS, 1500);
    pod_ui_back();
}

static void setup_remove_cb(lv_event_t *e)
{
    (void)e;
    if (!pod_cred_on_card()) {
        toast_info("There are none on the SD card");
        return;
    }
    if (pod_cred_save("", "")) {
        toast_show(pod_cred_built_in() ? "Removed; using the built-in key" : "Removed", TOAST_INFO, 2000);
        pod_ui_back();
    } else {
        toast_warn("Could not remove them");
    }
}

static void setup_key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        /* Enter goes on to the next field, as the Assistant's setup. */
        lv_obj_t *ta = lv_event_get_target(e);
        pod_ui_focus(ta == s_ui.key_ta ? s_ui.secret_ta : ta);
        if (ta == s_ui.secret_ta) setup_save_cb(e);
    }
}

static lv_obj_t *setup_field(lv_obj_t *col, const char *caption, bool password)
{
    const theme_colors_t *clr = theme_get_colors();
    text_label(col, caption, clr->text_secondary);
    lv_obj_t *ta = lv_textarea_create(col);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, password);
    lv_textarea_set_max_length(ta, password ? POD_CRED_SECRET_MAX - 1 : POD_CRED_KEY_MAX - 1);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_style_text_font(ta, &s_text_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, clr->text, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, clr->bg, LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, clr->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(ta, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(ta, 4, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, clr->text_secondary, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(ta, setup_key_cb, LV_EVENT_READY, NULL);
    return ta;
}

static void build_setup(page_t *p)
{
    const theme_colors_t *clr = theme_get_colors();
    lv_obj_t *page = pod_ui_page(p->title, NULL);
    s_ui.screen = page;

    lv_obj_t *col = lv_obj_create(page);
    lv_obj_set_pos(col, 0, POD_UI_TITLE_H);
    lv_obj_set_size(col, s_ui.w, s_ui.h - POD_UI_TITLE_H);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(col, POD_UI_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(col, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

    const char *where = pod_cred_on_card() ? "Using the key saved on the SD card."
                      : pod_cred_built_in() ? "Using the key built into this firmware."
                                            : "No key yet. Get one free at api.podcastindex.org.";
    text_label(col, where, clr->text);

    char key[POD_CRED_KEY_MAX] = "", secret[POD_CRED_SECRET_MAX] = "";
    bool have = pod_cred_on_card() && pod_cred_get(key, sizeof key, secret, sizeof secret);
    memset(secret, 0, sizeof secret);
    s_ui.key_ta = setup_field(col, "API key", false);
    lv_textarea_set_placeholder_text(s_ui.key_ta, "20 letters");
    if (have) lv_textarea_set_text(s_ui.key_ta, key);
    s_ui.secret_ta = setup_field(col, "API secret", true);
    lv_textarea_set_placeholder_text(s_ui.secret_ta, have ? "Saved; type to replace" : "40 characters");

    lv_obj_t *row = lv_obj_create(col);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, POD_UI_PAD, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *save = pod_ui_button(row, LV_SYMBOL_SAVE " Save", NULL, setup_save_cb, NULL);
    lv_obj_set_flex_grow(save, 1);
    lv_obj_t *remove = pod_ui_button(row, LV_SYMBOL_TRASH " Remove", NULL, setup_remove_cb, NULL);
    lv_obj_set_flex_grow(remove, 1);
    text_label(col, "Kept on the SD card in config/podcasts.json.", clr->text_secondary);

    lv_group_t *grp = lv_group_get_default();
    if (grp) {
        lv_group_add_obj(grp, s_ui.secret_ta);
        lv_group_add_obj(grp, save);
        lv_group_add_obj(grp, remove);
    }
    pod_ui_focus(s_ui.key_ta);
}

/* ------------------------------------------------------------------ */
/* Showing the page on top                                              */
/* ------------------------------------------------------------------ */

static void forget_widgets(void)
{
    s_ui.screen = NULL;
    s_ui.table = NULL;
    s_ui.star = NULL;
    s_ui.query_ta = s_ui.mode_btn = s_ui.clean_btn = NULL;
    s_ui.play_btn = s_ui.save_btn = s_ui.status_lbl = NULL;
    s_ui.key_ta = s_ui.secret_ta = NULL;
    s_ui.row_count = 0;
}

static void show_top(void)
{
    page_t *p = top();
    if (s_ui.screen) {
        lv_obj_delete(s_ui.screen);
        /* The player's widgets were on it. */
        pod_ui_player_release();
    }
    forget_widgets();
    if (!p) return;

    switch (p->kind) {
    case PAGE_HOME:    build_home(p); break;
    case PAGE_LIST:    build_list_page(p); break;
    case PAGE_SEARCH:  build_search(p); break;
    case PAGE_PODCAST: build_podcast(p); break;
    case PAGE_EPISODE: build_episode(p); break;
    case PAGE_SETUP:   build_setup(p); break;
    case PAGE_PLAYER:
        s_ui.screen = pod_ui_page(p->title, &s_ui.star);
        pod_ui_player_build(s_ui.screen, s_ui.star);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Keys every page takes                                                */
/* ------------------------------------------------------------------ */

bool pod_ui_common_key(uint32_t key)
{
    switch (key) {
    case 'q':
    case 'Q':
        if (!pod_ui_back()) app_manager_launch("com.thistle.launcher");
        return true;
    case 'p':
    case 'P':
    case 'n':
    case 'N': {
        pod_player_status_t st;
        pod_player_status(&st);
        if (!st.episode_id) return false;
        if (key == 'p' || key == 'P') pod_player_toggle();
        else pod_ui_open_player();
        return true;
    }
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* The tick                                                             */
/* ------------------------------------------------------------------ */

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    s_ui.ticks++;
    page_t *p = top();
    if (!p) return;

    /* Answers for pages beneath are kept for when they are shown again. */
    for (int i = 0; i < s_ui.depth - 1; i++) {
        page_t *below = s_ui.stack[i];
        if (take_job(below) && below->kind == PAGE_PODCAST && below->error[0]) podcast_offline(below);
    }
    if (take_job(p)) {
        if (p->kind == PAGE_PODCAST && p->error[0]) podcast_offline(p);
        fill_rows(p);
        select_row(0);
        if (p->kind == PAGE_SEARCH && s_ui.row_count && selectable(0)) {
            /* Results to choose from: the keys go to them. */
            pod_ui_focus(s_ui.table);
        }
    }

    /* On e-paper, a change is a refresh: progress moves every 5 s there. */
    uint32_t every = pod_ui_epaper() ? 20 : 4;
    switch (p->kind) {
    case PAGE_HOME: {
        pod_player_status_t st;
        pod_player_status(&st);
        if (st.state != s_ui.home_state || st.episode_id != s_ui.home_episode) {
            uint32_t row = 0, col = 0;
            lv_table_get_selected_cell(s_ui.table, &row, &col);
            bool had = s_ui.row_count && s_ui.rows[0].index == HOME_PLAYING;
            fill_home();
            bool has = s_ui.row_count && s_ui.rows[0].index == HOME_PLAYING;
            /* The same item stays selected as the Now playing row comes and goes. */
            int shift = (int)has - (int)had;
            select_row(row == LV_TABLE_CELL_NONE ? 0 : (uint32_t)((int)row + shift < 0 ? 0 : (int)row + shift));
        }
        break;
    }
    case PAGE_EPISODE:
        if (s_ui.confirm_ticks > 0 && --s_ui.confirm_ticks == 0) episode_buttons(p);
        if (s_ui.ticks % every == 0) episode_buttons(p);
        break;
    case PAGE_PLAYER:
        pod_ui_player_tick();
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

static void fonts_init(void)
{
    bool mono = pod_ui_epaper();
    s_symbol_font = mono ? pod_symbols_16_mono : pod_symbols_16;
    s_symbol_font.fallback = &lv_font_montserrat_14;
    s_text_font = mono ? reader_font_16_mono : reader_font_16;
    s_text_font.fallback = &s_symbol_font;
}

esp_err_t podcasts_ui_create(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "Creating Podcasts UI");
    if (!parent) parent = lv_scr_act();
    lv_obj_update_layout(parent);
    memset(&s_ui, 0, sizeof s_ui);
    s_ui.w = lv_obj_get_width(parent);
    s_ui.h = lv_obj_get_height(parent);
    if (s_ui.w <= 0) s_ui.w = 240;
    if (s_ui.h <= 0) s_ui.h = 296;
    fonts_init();

    /* The library is read once, the first time the app opens. */
    pod_lib_open(LIBRARY_DIR);

    s_ui.root = lv_obj_create(parent);
    lv_obj_set_size(s_ui.root, s_ui.w, s_ui.h);
    lv_obj_set_pos(s_ui.root, 0, 0);
    lv_obj_set_style_bg_opa(s_ui.root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_ui.root, LV_OBJ_FLAG_SCROLLABLE);
    ui_manager_set_back_handler(s_ui.root, back_handler);

    page_t *home = page_new(PAGE_HOME, "Podcasts");
    if (!home) {
        lv_obj_delete(s_ui.root);
        s_ui.root = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_ui.stack[s_ui.depth++] = home;
    show_top();
    s_ui.tick = lv_timer_create(tick_cb, TICK_MS, NULL);
    return ESP_OK;
}

void podcasts_ui_destroy(void)
{
    if (s_ui.tick) lv_timer_delete(s_ui.tick);
    pod_ui_player_release();
    while (s_ui.depth) page_free(s_ui.stack[--s_ui.depth]);
    if (s_ui.root) lv_obj_delete(s_ui.root);
    memset(&s_ui, 0, sizeof s_ui);
    pod_lib_flush();
}
