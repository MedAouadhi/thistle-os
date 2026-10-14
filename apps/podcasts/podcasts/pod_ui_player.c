/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Podcasts app's Now playing page.
 *
 *   < Now playing                     ☆
 *   The episode's title, in two lines
 *   Its podcast
 *   ━━━━━━━━━━━●────────────────────
 *   12:34       Streaming       -31:20
 *      [ -15 ]   [  ❚❚  ]   [ +30 ]
 *   [ - ]  50%  [ + ]        [ Save ]
 *
 * The bar seeks when let go. Keys: space plays or pauses, left and right go
 * back 15 s and on 30 s, up and down change the volume, s stars, d saves.
 *
 * Every change on e-paper is a refresh, so there the times move every 10 s
 * rather than every second; a change of state shows at once on both.
 */
#include "podcasts/pod_download.h"
#include "podcasts/pod_library.h"
#include "podcasts/pod_player.h"
#include "podcasts/pod_text.h"
#include "podcasts/pod_ui.h"

#include "thistle/audio_volume.h"
#include "ui/theme.h"
#include "ui/toast.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STAR        "\xE2\x98\x85"
#define STAR_HOLLOW "\xE2\x98\x86"
#define BACK_S      15
#define AHEAD_S     30
#define VOLUME_STEP 10

static EXT_RAM_BSS_ATTR struct {
    lv_obj_t *page;
    lv_obj_t *star;
    lv_obj_t *title;
    lv_obj_t *feed;
    lv_obj_t *bar;
    lv_obj_t *elapsed;
    lv_obj_t *status;
    lv_obj_t *left;
    lv_obj_t *play;
    lv_obj_t *volume;
    lv_obj_t *save;
    /* What is shown, to change only what changed */
    int64_t            episode_id;
    pod_player_state_t state;
    bool               buffering;
    uint32_t           ticks;
    bool               failed_told;
    pod_dl_state_t     dl_state;      /* what the Save button last showed */
} s_pl;

static EXT_RAM_BSS_ATTR pod_episode_t s_episode;
static EXT_RAM_BSS_ATTR pod_lib_episode_t s_known;

static void refresh(bool times);

static lv_obj_t *plain_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, POD_UI_PAD, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *label(lv_obj_t *parent, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, pod_ui_font(), LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    return l;
}

/* ------------------------------------------------------------------ */
/* Actions                                                              */
/* ------------------------------------------------------------------ */

static void play_cb(lv_event_t *e)
{
    (void)e;
    pod_player_toggle();
    refresh(true);
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    pod_player_skip(-BACK_S);
    refresh(true);
}

static void ahead_cb(lv_event_t *e)
{
    (void)e;
    pod_player_skip(AHEAD_S);
    refresh(true);
}

static void change_volume(int delta)
{
    int v = rs_audio_get_volume() + delta;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    /* Raised, it unmutes, as the quick panel's slider does. */
    if (delta > 0 && rs_audio_is_muted()) rs_audio_set_muted(false);
    rs_audio_set_volume((uint8_t)v, true);
    refresh(false);
}

static void volume_down_cb(lv_event_t *e)
{
    (void)e;
    change_volume(-VOLUME_STEP);
}

static void volume_up_cb(lv_event_t *e)
{
    (void)e;
    change_volume(VOLUME_STEP);
}

static void star_cb(lv_event_t *e)
{
    (void)e;
    if (!pod_player_episode(&s_episode)) return;
    pod_lib_episode(s_episode.id, &s_known);
    bool starred = !s_known.starred_at;
    pod_lib_star_episode(&s_episode, starred, (int64_t)time(NULL));
    pod_lib_flush();
    toast_show(starred ? "Starred" : "No longer starred", TOAST_INFO, 1500);
    refresh(false);
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    if (!pod_player_episode(&s_episode)) return;
    pod_dl_status_t dl;
    pod_download_status(&s_episode, &dl);
    if (dl.state == POD_DL_QUEUED || dl.state == POD_DL_RUNNING) {
        pod_download_cancel(s_episode.id);
        toast_show("Not saved", TOAST_INFO, 1500);
    } else if (dl.state == POD_DL_SAVED) {
        toast_show("Saved on the SD card", TOAST_INFO, 1500);
    } else {
        const char *why = pod_download_start(&s_episode);
        if (why) toast_warn(why);
        else toast_show("Saving to the SD card", TOAST_INFO, 1500);
    }
    refresh(false);
}

/* The bar seeks when it is let go; while dragged, the time it is at shows. */
static void bar_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int32_t value = lv_slider_get_value(s_pl.bar);
    if (code == LV_EVENT_VALUE_CHANGED && lv_slider_is_dragged(s_pl.bar)) {
        char at[16];
        pod_text_clock(value, at, sizeof at);
        pod_ui_set_text(s_pl.elapsed, at);
    } else if (code == LV_EVENT_RELEASED) {
        pod_player_seek(value);
        refresh(true);
    }
}

bool pod_ui_player_key(uint32_t key)
{
    switch (key) {
    case ' ':
        pod_player_toggle();
        break;
    case LV_KEY_LEFT:
    case 'b':
    case ',':
        pod_player_skip(-BACK_S);
        break;
    case LV_KEY_RIGHT:
    case 'f':
    case '.':
        pod_player_skip(AHEAD_S);
        break;
    case LV_KEY_UP:
    case '+':
    case '=':
        change_volume(VOLUME_STEP);
        return true;
    case LV_KEY_DOWN:
    case '-':
        change_volume(-VOLUME_STEP);
        return true;
    case 's':
        star_cb(NULL);
        return true;
    case 'd':
        save_cb(NULL);
        return true;
    default:
        return pod_ui_common_key(key);
    }
    refresh(true);
    return true;
}

static void key_cb(lv_event_t *e)
{
    pod_ui_player_key(lv_event_get_key(e));
}

/* ------------------------------------------------------------------ */
/* Showing it                                                           */
/* ------------------------------------------------------------------ */

static void refresh(bool times)
{
    if (!s_pl.page) return;
    pod_player_status_t st;
    pod_player_status(&st);
    bool loaded = pod_player_episode(&s_episode);

    if (st.episode_id != s_pl.episode_id) {
        s_pl.episode_id = st.episode_id;
        s_pl.failed_told = false;
        pod_ui_set_text(s_pl.title, loaded ? s_episode.title : "Nothing is playing");
        const char *feed = loaded ? s_episode.feed_title : "";
        pod_lib_feed_t lf;
        if (loaded && !feed[0] && pod_lib_feed(s_episode.feed_id, &lf)) feed = lf.feed.title;
        pod_ui_set_text(s_pl.feed, loaded ? feed : "Choose an episode and press Play.");
        times = true;
    }
    if (st.state != s_pl.state || st.buffering != s_pl.buffering) times = true;
    s_pl.state = st.state;
    s_pl.buffering = st.buffering;

    /* Play or pause: what pressing it would do. */
    bool on = st.state == POD_PLAYER_PLAYING || st.state == POD_PLAYER_LOADING;
    pod_ui_set_text(pod_ui_button_label(s_pl.play),
                    on ? LV_SYMBOL_PAUSE : st.state == POD_PLAYER_ENDED ? LV_SYMBOL_REFRESH : LV_SYMBOL_PLAY);

    if (times) {
        const char *status;
        switch (st.state) {
        case POD_PLAYER_LOADING: status = "Loading" "\xE2\x80\xA6"; break;
        case POD_PLAYER_PLAYING:
            status = st.buffering ? "Buffering" "\xE2\x80\xA6" : st.local ? "From the card" : "Streaming";
            break;
        case POD_PLAYER_PAUSED:  status = "Paused"; break;
        case POD_PLAYER_ENDED:   status = "Played"; break;
        case POD_PLAYER_FAILED:  status = "Stopped"; break;
        default:                 status = ""; break;
        }
        pod_ui_set_text(s_pl.status, status);
        if (st.state == POD_PLAYER_FAILED && !s_pl.failed_told && st.error[0]) {
            toast_warn(st.error);
            s_pl.failed_told = true;
        }

        if (!lv_slider_is_dragged(s_pl.bar)) {
            char at[16], left[20];
            pod_text_clock(st.position, at, sizeof at);
            pod_ui_set_text(s_pl.elapsed, at);
            if (st.duration > 0) {
                left[0] = '-';
                pod_text_clock(st.duration - st.position, left + 1, sizeof left - 1);
            } else {
                left[0] = '\0';
            }
            pod_ui_set_text(s_pl.left, left);
            int32_t max = st.duration > 0 ? st.duration : 1;
            if (lv_slider_get_max_value(s_pl.bar) != max) lv_slider_set_range(s_pl.bar, 0, max);
            int32_t value = st.position < max ? st.position : max;
            if (lv_slider_get_value(s_pl.bar) != value) lv_slider_set_value(s_pl.bar, value, LV_ANIM_OFF);
        }
    }

    char text[48];
    if (rs_audio_available()) {
        if (rs_audio_is_muted()) snprintf(text, sizeof text, LV_SYMBOL_MUTE);
        else snprintf(text, sizeof text, "%u%%", (unsigned)rs_audio_get_volume());
    } else {
        snprintf(text, sizeof text, "--");
    }
    pod_ui_set_text(s_pl.volume, text);

    if (loaded) {
        pod_lib_episode(s_episode.id, &s_known);
        pod_ui_set_text(pod_ui_button_label(s_pl.star), s_known.starred_at ? STAR : STAR_HOLLOW);
        pod_dl_status_t dl;
        pod_download_status(&s_episode, &dl);
        /* A download's percentage moves several times a second, and on
         * e-paper each change is a refresh: there it moves with the times,
         * and at once only when the download starts, ends or fails. */
        bool moved = dl.state != s_pl.dl_state;
        s_pl.dl_state = dl.state;
        if (!moved && !times && dl.state == POD_DL_RUNNING && pod_ui_epaper()) return;
        switch (dl.state) {
        case POD_DL_QUEUED:
            snprintf(text, sizeof text, LV_SYMBOL_CLOSE " Waiting");
            break;
        case POD_DL_RUNNING:
            snprintf(text, sizeof text, LV_SYMBOL_CLOSE " %d%%",
                     dl.total > 0 ? (int)(dl.done * 100 / dl.total) : 0);
            break;
        case POD_DL_SAVED:
            snprintf(text, sizeof text, LV_SYMBOL_SD_CARD " Saved");
            break;
        default:
            snprintf(text, sizeof text, LV_SYMBOL_DOWNLOAD " Save");
            break;
        }
        pod_ui_set_text(pod_ui_button_label(s_pl.save), text);
    } else {
        pod_ui_set_text(pod_ui_button_label(s_pl.star), "");
        pod_ui_set_text(pod_ui_button_label(s_pl.save), LV_SYMBOL_DOWNLOAD " Save");
    }
}

void pod_ui_player_tick(void)
{
    if (!s_pl.page) return;
    s_pl.ticks++;
    /* Every second on a colour screen; every 10 s on e-paper, where each
     * change costs a refresh. A new state shows at once on both. */
    uint32_t every = pod_ui_epaper() ? 40 : 4;
    refresh(s_pl.ticks % every == 0);
}

void pod_ui_player_build(lv_obj_t *page, lv_obj_t *star)
{
    const theme_colors_t *clr = theme_get_colors();
    memset(&s_pl, 0, sizeof s_pl);
    s_pl.page = page;
    s_pl.star = star;
    s_pl.episode_id = -1;
    s_pl.state = (pod_player_state_t)-1;
    int32_t w = pod_ui_width(), h = pod_ui_height();
    /* A landscape screen has room for one line of title. */
    bool short_screen = h - POD_UI_TITLE_H < 220;
    const lv_font_t *font = pod_ui_font();
    int32_t line = lv_font_get_line_height(font);

    lv_obj_t *col = lv_obj_create(page);
    lv_obj_set_pos(col, 0, POD_UI_TITLE_H);
    lv_obj_set_size(col, w, h - POD_UI_TITLE_H);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(col, POD_UI_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(col, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_AUTO);

    s_pl.title = label(col, clr->text);
    lv_label_set_long_mode(s_pl.title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_pl.title, LV_PCT(100), line * (short_screen ? 1 : 2));
    s_pl.feed = label(col, clr->text_secondary);
    lv_label_set_long_mode(s_pl.feed, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_pl.feed, LV_PCT(100));

    s_pl.bar = lv_slider_create(col);
    lv_obj_set_width(s_pl.bar, w - 2 * POD_UI_PAD - 12);
    lv_obj_set_height(s_pl.bar, 6);
    lv_obj_set_style_margin_hor(s_pl.bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_pl.bar, clr->surface, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_pl.bar, clr->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pl.bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_pl.bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_pl.bar, clr->text, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_pl.bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_pl.bar, clr->text, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_pl.bar, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(s_pl.bar, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_pl.bar, 4, LV_PART_KNOB);
    lv_slider_set_range(s_pl.bar, 0, 1);
    /* Its keys are the page's: left and right skip rather than nudge. */
    lv_group_remove_obj(s_pl.bar);
    lv_obj_add_event_cb(s_pl.bar, bar_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_pl.bar, bar_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_flag(s_pl.bar, LV_OBJ_FLAG_ADV_HITTEST);

    lv_obj_t *times = plain_row(col);
    s_pl.elapsed = label(times, clr->text);
    s_pl.status = label(times, clr->text_secondary);
    lv_label_set_long_mode(s_pl.status, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(s_pl.status, 1);
    lv_obj_set_style_text_align(s_pl.status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    s_pl.left = label(times, clr->text);
    lv_obj_set_style_text_align(s_pl.left, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    lv_obj_t *controls = plain_row(col);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *back = pod_ui_button(controls, "-15", &lv_font_montserrat_18, back_cb, NULL);
    s_pl.play = pod_ui_button(controls, LV_SYMBOL_PLAY, &lv_font_montserrat_22, play_cb, NULL);
    lv_obj_set_width(s_pl.play, w / 3);
    lv_obj_t *ahead = pod_ui_button(controls, "+30", &lv_font_montserrat_18, ahead_cb, NULL);

    lv_obj_t *bottom = plain_row(col);
    lv_obj_t *down = pod_ui_button(bottom, LV_SYMBOL_MINUS, &lv_font_montserrat_14, volume_down_cb, NULL);
    s_pl.volume = label(bottom, clr->text);
    lv_obj_set_style_text_align(s_pl.volume, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_pl.volume, 44);
    lv_obj_t *up = pod_ui_button(bottom, LV_SYMBOL_PLUS, &lv_font_montserrat_14, volume_up_cb, NULL);
    s_pl.save = pod_ui_button(bottom, "", NULL, save_cb, NULL);
    lv_obj_set_flex_grow(s_pl.save, 1);

    lv_obj_t *keyed[] = {s_pl.play, back, ahead, down, up, s_pl.save, star};
    for (size_t i = 0; i < sizeof keyed / sizeof keyed[0]; i++) {
        if (keyed[i]) lv_obj_add_event_cb(keyed[i], key_cb, LV_EVENT_KEY, NULL);
    }
    if (star) lv_obj_add_event_cb(star, star_cb, LV_EVENT_CLICKED, NULL);

    refresh(true);
    pod_ui_focus(s_pl.play);
}

void pod_ui_player_release(void)
{
    memset(&s_pl, 0, sizeof s_pl);
}
