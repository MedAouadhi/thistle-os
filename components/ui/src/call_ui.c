/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the call screen
 *
 * The kernel runs every call (thistle/call.h). This follows it: an LVGL
 * timer reads rs_call_get_status() four times a second and opens the screen
 * when a call rings here or is placed, shows what changes, and closes it once
 * the kernel has said how the call ended.
 *
 * Like the quick panel, it is built when it opens and deleted when it closes,
 * so it costs LVGL's 64 KB pool nothing between calls, or while a call is
 * tucked away; everything in it shares a handful of styles. A label is only
 * rewritten when its text changes, and on e-paper the connected time only
 * every 5 s: each rewrite there is a refresh of the panel. "On" is drawn
 * inverted, which reads in black and white.
 *
 * It lies over the app in front without touching it, below the status bar,
 * which keeps showing. Its own group takes the keyboard while it is open.
 *
 * Everything here runs on the LVGL task with the LVGL lock held: from its
 * timer, its buttons, or the UI manager's key timer.
 */

#include "ui/call_ui.h"
#include "ui/manager.h"
#include "ui/power_ui.h"
#include "ui/quick_panel.h"
#include "ui/statusbar.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "hal/board.h"
#include "thistle/app_manager.h"
#include "thistle/call.h"

#include "esp_log.h"
#include "lvgl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "call_ui";

#define PHONE_APP_ID "com.thistle.phone"

#define TICK_MS       250
#define VOLUME_STEP   15
/* On e-paper the connected time moves on in steps this long. */
#define EPAPER_TIME_STEP_S 5
/* The DTMF digits sent, as many of the latest as are shown. */
#define DIGITS_SHOWN  20

#define PAD      6
#define PAD_TOP 10
#define GAP      5
#define RADIUS   8

/* Heights of the controls: on a tall screen (the T-Deck Max's 240 x 296
 * below the status bar) and on a wide one (320 x 216), where a toggle puts
 * its symbol beside its word. */
typedef struct {
    int32_t toggle_h;
    int32_t volume_h;
    int32_t end_h;
    int32_t answer_h;
    int32_t small_h;
    int32_t step_w;    /* volume - and + */
    int32_t hide_w;    /* the keypad's Hide, beside End */
} sizes_t;

static const sizes_t TALL = { 48, 32, 40, 64, 32, 56, 72 };
static const sizes_t WIDE = { 34, 30, 36, 56, 30, 56, 0 };

typedef enum {
    MODE_NONE = 0,
    MODE_RINGING,   /* Decline, Answer, Silence */
    MODE_CALL,      /* Mute, Keypad, Speaker, volume, End call */
    MODE_KEYPAD,    /* the DTMF keys, Hide, End call */
    MODE_ENDED,     /* nothing to press: the status line says how it ended */
} screen_mode_t;

/* The screen while it is open. */
static struct {
    lv_obj_t     *root;
    lv_obj_t     *status;    /* "Incoming call", "Calling...", the time */
    lv_obj_t     *name;      /* the name, or the number without one */
    lv_obj_t     *number;    /* the number, under a name */
    lv_obj_t     *controls;  /* the mode's buttons */
    lv_obj_t     *silence;   /* the Silence button's label */
    lv_obj_t     *mute;
    lv_obj_t     *speaker;
    lv_obj_t     *volume;    /* the volume's label */
    lv_obj_t     *digits;    /* the DTMF digits sent */
    /* What the name and number labels were last given. A label cut short
     * with dots holds the dotted text, so it cannot be asked. */
    char          name_shown[CALL_NAME_MAX];
    char          number_shown[CALL_NUMBER_MAX];
    bool          name_in_text_font;   /* a name, in the apps layer's font */
    screen_mode_t mode;
    call_status_t st;        /* as last shown */
    bool          wide;
    const sizes_t *sz;
    lv_group_t   *group;
    lv_indev_t   *keypad;
    lv_group_t   *keypad_group_before;
} s_ui;

static lv_timer_t *s_timer;
static volatile bool s_open;
/* The call is up but its screen closed, by Back, Home or Esc. */
static bool s_tucked;
/* This call's ringing was silenced here. */
static bool s_silenced;
/* This call's DTMF keys are shown in place of its toggles. */
static bool s_keypad_shown;
static char s_digits[DIGITS_SHOWN + 1];
static call_state_t s_last_state = CALL_IDLE;
static uint32_t s_missed_logged;
static bool s_epaper;

/* Names from the phonebook: call_ui_set_text_font(). */
static lv_font_t s_text_font;
static bool s_have_text_font;
static call_ui_shape_fn s_shape;

static lv_style_t s_style_root;
static lv_style_t s_style_box;        /* a plain container: no fill, no border, no padding */
static lv_style_t s_style_grow;
static lv_style_t s_style_button;
static lv_style_t s_style_inverted;   /* Answer, End call, and a toggle that is on */
static lv_style_t s_style_pressed;
static lv_style_t s_style_focus;
static lv_style_t s_style_text;       /* 14 */
static lv_style_t s_style_symbol;     /* 18 */
static lv_style_t s_style_big_symbol; /* 22 */
static lv_style_t s_style_line;       /* one centred line of 14, as wide as its parent */
static lv_style_t s_style_name;       /* one centred line of 22, as wide as its parent */
static lv_style_t s_style_name_text;  /* the same in s_text_font, for a name */
static lv_style_t s_style_keys;       /* the DTMF keys' matrix */

static void key_cb(lv_event_t *e);
static void build_controls(screen_mode_t mode);

/* ------------------------------------------------------------------ */
/* The status                                                           */
/* ------------------------------------------------------------------ */

#ifdef SIMULATOR_BUILD
static bool s_previewing;
static call_status_t s_preview;
static uint32_t s_preview_since;
/* The "ended" preview: connected at first, so the screen opens, then ended. */
static bool s_preview_ends;
#define PREVIEW_ENDS_MS 1500
#endif

static void get_status(call_status_t *out)
{
#ifdef SIMULATOR_BUILD
    if (s_previewing) {
        *out = s_preview;
        uint32_t elapsed = lv_tick_elaps(s_preview_since);
        if (s_preview_ends && elapsed >= PREVIEW_ENDS_MS) {
            out->state = CALL_ENDED;
            out->end = CALL_END_REMOTE;
            out->seq++;
        } else if (out->state == CALL_ACTIVE) {
            out->duration_s += elapsed / 1000;
        }
        return;
    }
#endif
    rs_call_get_status(out);
}

/* Ringing here, being placed, or connected. */
static bool is_live(call_state_t state)
{
    return state == CALL_INCOMING || state == CALL_DIALING || state == CALL_ALERTING ||
           state == CALL_ACTIVE;
}

static bool display_is_epaper(void)
{
    const hal_registry_t *reg = hal_get_registry();
    return reg && reg->display && reg->display->type == HAL_DISPLAY_TYPE_EPAPER;
}

/* "01:23", or "1:02:03" past an hour. */
static void format_duration(uint32_t seconds, char *out, size_t size)
{
    unsigned h = (unsigned)(seconds / 3600);
    unsigned m = (unsigned)(seconds / 60 % 60);
    unsigned s = (unsigned)(seconds % 60);
    if (h) {
        snprintf(out, size, "%u:%02u:%02u", h, m, s);
    } else {
        snprintf(out, size, "%02u:%02u", m, s);
    }
}

/* What is happening, for the line at the top. */
static void status_line(const call_status_t *st, char *out, size_t size)
{
    char time[16];
    switch (st->state) {
    case CALL_INCOMING: snprintf(out, size, "Incoming call"); break;
    case CALL_DIALING:  snprintf(out, size, "Calling..."); break;
    case CALL_ALERTING: snprintf(out, size, "Ringing..."); break;
    case CALL_ACTIVE: {
        uint32_t d = st->duration_s;
        if (s_epaper) d -= d % EPAPER_TIME_STEP_S;
        format_duration(d, out, size);
        break;
    }
    case CALL_ENDED:
        switch (st->end) {
        case CALL_END_MISSED:   snprintf(out, size, "Missed call"); break;
        case CALL_END_REJECTED: snprintf(out, size, "Declined"); break;
        case CALL_END_FAILED:   snprintf(out, size, "Call failed"); break;
        default:
            if (st->duration_s) {
                /* U+2022, a bullet: the fonts carry it, not a middle dot. */
                format_duration(st->duration_s, time, sizeof(time));
                snprintf(out, size, "Call ended \xE2\x80\xA2 %s", time);
            } else {
                snprintf(out, size, "Call ended");
            }
            break;
        }
        break;
    default:
        out[0] = '\0';
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Showing it                                                           */
/* ------------------------------------------------------------------ */

static void set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

/* For a label cut short with dots: compared with what it was given. */
static void set_dotted_text(lv_obj_t *label, char *shown, size_t size, const char *text)
{
    if (label && strcmp(shown, text) != 0) {
        snprintf(shown, size, "%s", text);
        lv_label_set_text(label, text);
    }
}

static void set_checked(lv_obj_t *obj, bool on)
{
    if (!obj || on == lv_obj_has_state(obj, LV_STATE_CHECKED)) return;
    if (on) {
        lv_obj_add_state(obj, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(obj, LV_STATE_CHECKED);
    }
}

static void show_volume(uint8_t percent)
{
    if (!s_ui.volume) return;
    char text[24];
    snprintf(text, sizeof(text), LV_SYMBOL_VOLUME_MAX " %u%%", (unsigned)percent);
    set_text_if_changed(s_ui.volume, text);
}

static void show_status(const call_status_t *st)
{
    char line[40];
    status_line(st, line, sizeof(line));
    set_text_if_changed(s_ui.status, line);
}

/* A name in the apps layer's font, shaped, if it gave one; a number, or a
 * name without it, in Montserrat 22. */
static void show_big(const char *text, bool is_name)
{
    bool text_font = is_name && s_have_text_font;
    if (text_font != s_ui.name_in_text_font) {
        lv_obj_remove_style(s_ui.name, text_font ? &s_style_name : &s_style_name_text,
                            LV_PART_MAIN);
        lv_obj_add_style(s_ui.name, text_font ? &s_style_name_text : &s_style_name,
                         LV_PART_MAIN);
        s_ui.name_in_text_font = text_font;
    }
    if (strcmp(s_ui.name_shown, text) == 0) return;
    snprintf(s_ui.name_shown, sizeof(s_ui.name_shown), "%s", text);
    char *shaped = is_name && s_shape ? s_shape(text) : NULL;
    lv_label_set_text(s_ui.name, shaped ? shaped : text);
    free(shaped);
}

/* The name large, the number under it; without a name, the number large;
 * without a number, "Unknown". */
static void show_who(const call_status_t *st)
{
    bool is_name = st->name[0] != '\0';
    show_big(is_name ? st->name : st->number[0] ? st->number : "Unknown", is_name);
    set_dotted_text(s_ui.number, s_ui.number_shown, sizeof(s_ui.number_shown),
                    is_name ? st->number : "");
}

static screen_mode_t mode_for(call_state_t state)
{
    switch (state) {
    case CALL_INCOMING: return MODE_RINGING;
    case CALL_DIALING:
    case CALL_ALERTING:
    case CALL_ACTIVE:   return s_keypad_shown ? MODE_KEYPAD : MODE_CALL;
    case CALL_ENDED:    return MODE_ENDED;
    default:            return MODE_NONE;
    }
}

static void apply(const call_status_t *st)
{
    s_ui.st = *st;
    screen_mode_t mode = mode_for(st->state);
    if (mode != s_ui.mode) {
        build_controls(mode);
        if (mode == MODE_ENDED) {
            char line[40];
            status_line(st, line, sizeof(line));
            ESP_LOGI(TAG, "ended shown: %s", line);
        }
    }
    show_who(st);
    show_status(st);
    set_checked(s_ui.mute, st->muted);
    set_checked(s_ui.speaker, st->speaker);
    show_volume(st->volume);
}

/* ------------------------------------------------------------------ */
/* Acting                                                               */
/* ------------------------------------------------------------------ */

static void log_result(const char *what, esp_err_t err)
{
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s", what);
    } else {
        ESP_LOGW(TAG, "%s: %s", what, esp_err_to_name(err));
    }
}

static void silence(void)
{
    rs_call_silence();
    s_silenced = true;
    set_text_if_changed(s_ui.silence, LV_SYMBOL_MUTE " Silenced");
    ESP_LOGI(TAG, "silenced");
}

static void send_dtmf(char digit)
{
    esp_err_t err = rs_call_dtmf(digit);
    char what[16];
    snprintf(what, sizeof(what), "dtmf %c", digit);
    log_result(what, err);
    if (err != ESP_OK) return;

    size_t n = strlen(s_digits);
    if (n == DIGITS_SHOWN) {
        memmove(s_digits, s_digits + 1, n);
        n--;
    }
    s_digits[n] = digit;
    s_digits[n + 1] = '\0';
    set_text_if_changed(s_ui.digits, s_digits);
}

static void answer_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = rs_call_answer();
    log_result("answered", err);
    /* The call reads INCOMING until the modem lists it connected, up to a
     * second: say the tap was taken. The status's next change rewrites it. */
    if (err == ESP_OK) {
        set_text_if_changed(s_ui.status, "Answering...");
    }
}

static void decline_cb(lv_event_t *e)
{
    (void)e;
    log_result("declined", rs_call_hangup());
}

static void end_cb(lv_event_t *e)
{
    (void)e;
    log_result("end call", rs_call_hangup());
}

static void silence_cb(lv_event_t *e)
{
    (void)e;
    silence();
}

static void mute_cb(lv_event_t *e)
{
    (void)e;
    bool on = !lv_obj_has_state(s_ui.mute, LV_STATE_CHECKED);
    esp_err_t err = rs_call_set_mute(on);
    log_result(on ? "mute on" : "mute off", err);
    if (err == ESP_OK) {
        set_checked(s_ui.mute, on);
        s_ui.st.muted = on;
    }
}

static void speaker_cb(lv_event_t *e)
{
    (void)e;
    bool on = !lv_obj_has_state(s_ui.speaker, LV_STATE_CHECKED);
    esp_err_t err = rs_call_set_speaker(on);
    log_result(on ? "speaker on" : "speaker off", err);
    if (err == ESP_OK) {
        set_checked(s_ui.speaker, on);
        s_ui.st.speaker = on;
    }
}

/* Volume down or up a step, from what the screen shows, so two quick presses
 * make two steps even before the kernel reports the first. */
static void volume_cb(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    int level = (int)s_ui.st.volume + step * VOLUME_STEP;
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    if (level == s_ui.st.volume) return;

    esp_err_t err = rs_call_set_volume((uint8_t)level);
    char what[24];
    snprintf(what, sizeof(what), "volume %d%%", level);
    log_result(what, err);
    if (err == ESP_OK) {
        s_ui.st.volume = (uint8_t)level;
        show_volume((uint8_t)level);
    }
}

static void keypad_cb(lv_event_t *e)
{
    (void)e;
    s_keypad_shown = true;
    build_controls(MODE_KEYPAD);
}

static void hide_keypad_cb(lv_event_t *e)
{
    (void)e;
    s_keypad_shown = false;
    build_controls(MODE_CALL);
}

static void dtmf_key_cb(lv_event_t *e)
{
    lv_obj_t *keys = lv_event_get_target(e);
    const uint32_t *id = lv_event_get_param(e);
    uint32_t button = id ? *id : lv_buttonmatrix_get_selected_button(keys);
    if (button == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *text = lv_buttonmatrix_get_button_text(keys, button);
    if (text && text[0]) send_dtmf(text[0]);
}

static bool is_dtmf_key(uint32_t key)
{
    return (key >= '0' && key <= '9') || key == '*' || key == '#';
}

/* The keyboard: digits for DTMF while the keypad shows, and the arrows to
 * move between buttons, which plain objects do not do by themselves. */
static void key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (s_ui.mode == MODE_KEYPAD && is_dtmf_key(key)) {
        send_dtmf((char)key);
        return;
    }
    if (!s_ui.group || lv_obj_check_type(lv_event_get_target(e), &lv_buttonmatrix_class)) {
        return;  /* the matrix moves its own selection */
    }
    if (key == LV_KEY_LEFT || key == LV_KEY_UP) {
        lv_group_focus_prev(s_ui.group);
    } else if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN) {
        lv_group_focus_next(s_ui.group);
    }
}

/* ------------------------------------------------------------------ */
/* Building it                                                          */
/* ------------------------------------------------------------------ */

static void styles_init(void)
{
    const theme_colors_t *tc = theme_get_colors();

    lv_style_reset(&s_style_root);
    lv_style_init(&s_style_root);
    lv_style_set_bg_color(&s_style_root, tc->bg);
    lv_style_set_bg_opa(&s_style_root, LV_OPA_COVER);
    lv_style_set_text_color(&s_style_root, tc->text);
    lv_style_set_pad_all(&s_style_root, PAD);
    lv_style_set_pad_top(&s_style_root, PAD_TOP);
    lv_style_set_pad_row(&s_style_root, GAP);
    lv_style_set_layout(&s_style_root, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_style_root, LV_FLEX_FLOW_COLUMN);
    lv_style_set_flex_main_place(&s_style_root, LV_FLEX_ALIGN_START);
    lv_style_set_flex_cross_place(&s_style_root, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&s_style_root, LV_FLEX_ALIGN_CENTER);

    lv_style_reset(&s_style_box);
    lv_style_init(&s_style_box);
    lv_style_set_bg_opa(&s_style_box, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_style_box, 0);
    lv_style_set_pad_all(&s_style_box, 0);
    lv_style_set_pad_row(&s_style_box, GAP);
    lv_style_set_pad_column(&s_style_box, GAP);
    lv_style_set_width(&s_style_box, LV_PCT(100));
    lv_style_set_height(&s_style_box, LV_SIZE_CONTENT);
    lv_style_set_layout(&s_style_box, LV_LAYOUT_FLEX);

    lv_style_reset(&s_style_grow);
    lv_style_init(&s_style_grow);
    lv_style_set_flex_grow(&s_style_grow, 1);

    lv_style_reset(&s_style_button);
    lv_style_init(&s_style_button);
    lv_style_set_bg_color(&s_style_button, tc->bg);
    lv_style_set_bg_opa(&s_style_button, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_button, tc->text);
    lv_style_set_border_width(&s_style_button, 1);
    lv_style_set_radius(&s_style_button, RADIUS);
    lv_style_set_pad_all(&s_style_button, 2);
    lv_style_set_pad_row(&s_style_button, 0);
    lv_style_set_pad_column(&s_style_button, 6);
    lv_style_set_text_color(&s_style_button, tc->text);
    lv_style_set_layout(&s_style_button, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_style_button, LV_FLEX_FLOW_COLUMN);
    lv_style_set_flex_main_place(&s_style_button, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_cross_place(&s_style_button, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&s_style_button, LV_FLEX_ALIGN_CENTER);

    lv_style_reset(&s_style_inverted);
    lv_style_init(&s_style_inverted);
    lv_style_set_bg_color(&s_style_inverted, tc->text);
    lv_style_set_text_color(&s_style_inverted, tc->bg);

    lv_style_reset(&s_style_pressed);
    lv_style_init(&s_style_pressed);
    lv_style_set_bg_color(&s_style_pressed, tc->primary);
    lv_style_set_text_color(&s_style_pressed, tc->bg);

    /* The keyboard's focus: an outline, which shows around a filled button
     * as well as an outlined one. */
    lv_style_reset(&s_style_focus);
    lv_style_init(&s_style_focus);
    lv_style_set_outline_width(&s_style_focus, 2);
    lv_style_set_outline_color(&s_style_focus, tc->text);
    lv_style_set_outline_pad(&s_style_focus, 2);

    lv_style_reset(&s_style_text);
    lv_style_init(&s_style_text);
    lv_style_set_text_font(&s_style_text, &lv_font_montserrat_14);

    lv_style_reset(&s_style_symbol);
    lv_style_init(&s_style_symbol);
    lv_style_set_text_font(&s_style_symbol, &lv_font_montserrat_18);

    lv_style_reset(&s_style_big_symbol);
    lv_style_init(&s_style_big_symbol);
    lv_style_set_text_font(&s_style_big_symbol, &lv_font_montserrat_22);

    lv_style_reset(&s_style_line);
    lv_style_init(&s_style_line);
    lv_style_set_text_font(&s_style_line, &lv_font_montserrat_14);
    lv_style_set_text_align(&s_style_line, LV_TEXT_ALIGN_CENTER);
    lv_style_set_width(&s_style_line, LV_PCT(100));
    lv_style_set_height(&s_style_line, lv_font_get_line_height(&lv_font_montserrat_14));

    lv_style_reset(&s_style_name);
    lv_style_init(&s_style_name);
    lv_style_set_text_font(&s_style_name, &lv_font_montserrat_22);
    lv_style_set_text_align(&s_style_name, LV_TEXT_ALIGN_CENTER);
    lv_style_set_width(&s_style_name, LV_PCT(100));
    lv_style_set_height(&s_style_name, lv_font_get_line_height(&lv_font_montserrat_22));

    /* No letter spacing to set it apart: it would pull joined Arabic letters
     * apart. It is the largest text here as it is. */
    lv_style_reset(&s_style_name_text);
    lv_style_init(&s_style_name_text);
    if (s_have_text_font) {
        lv_style_set_text_font(&s_style_name_text, &s_text_font);
        lv_style_set_text_align(&s_style_name_text, LV_TEXT_ALIGN_CENTER);
        lv_style_set_width(&s_style_name_text, LV_PCT(100));
        lv_style_set_height(&s_style_name_text, lv_font_get_line_height(&s_text_font));
    }

    lv_style_reset(&s_style_keys);
    lv_style_init(&s_style_keys);
    lv_style_set_bg_opa(&s_style_keys, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_style_keys, 0);
    lv_style_set_pad_all(&s_style_keys, 0);
    lv_style_set_pad_row(&s_style_keys, 4);
    lv_style_set_pad_column(&s_style_keys, GAP);
}

static lv_obj_t *add_box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &s_style_box, LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(box, flow);
    return box;
}

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, lv_style_t *style)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_obj_add_style(label, style, LV_PART_MAIN);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *add_button(lv_obj_t *parent, lv_event_cb_t cb, void *user_data, bool inverted)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &s_style_button, LV_PART_MAIN);
    if (inverted) {
        lv_obj_add_style(btn, &s_style_inverted, LV_PART_MAIN);
    }
    lv_obj_add_style(btn, &s_style_inverted, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_style(btn, &s_style_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(btn, &s_style_focus, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_add_event_cb(btn, key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(s_ui.group, btn);
    return btn;
}

/* Mute, Keypad or Speaker: its symbol over its word, or beside it on a wide
 * screen. */
static lv_obj_t *add_toggle(lv_obj_t *row, const char *symbol, const char *word,
                            lv_event_cb_t cb)
{
    lv_obj_t *btn = add_button(row, cb, NULL, false);
    lv_obj_add_style(btn, &s_style_grow, LV_PART_MAIN);
    lv_obj_set_height(btn, LV_PCT(100));
    if (s_ui.wide) {
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    }
    add_label(btn, symbol, &s_style_symbol);
    add_label(btn, word, &s_style_text);
    return btn;
}

static lv_obj_t *build_ringing(lv_obj_t *box)
{
    const sizes_t *sz = s_ui.sz;

    lv_obj_t *silence_btn = add_button(box, silence_cb, NULL, false);
    lv_obj_set_size(silence_btn, LV_SIZE_CONTENT, sz->small_h);
    lv_obj_set_style_pad_hor(silence_btn, 14, LV_PART_MAIN);
    s_ui.silence = add_label(silence_btn,
                             s_silenced ? LV_SYMBOL_MUTE " Silenced" : LV_SYMBOL_MUTE " Silence",
                             &s_style_text);

    lv_obj_t *row = add_box(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(row, sz->answer_h);

    lv_obj_t *decline = add_button(row, decline_cb, NULL, false);
    lv_obj_add_style(decline, &s_style_grow, LV_PART_MAIN);
    lv_obj_set_height(decline, LV_PCT(100));
    add_label(decline, LV_SYMBOL_CLOSE, &s_style_big_symbol);
    add_label(decline, "Decline", &s_style_text);

    lv_obj_t *answer = add_button(row, answer_cb, NULL, true);
    lv_obj_add_style(answer, &s_style_grow, LV_PART_MAIN);
    lv_obj_set_height(answer, LV_PCT(100));
    add_label(answer, LV_SYMBOL_CALL, &s_style_big_symbol);
    add_label(answer, "Answer", &s_style_text);
    return answer;
}

static lv_obj_t *add_end_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *end = add_button(parent, end_cb, NULL, true);
    add_label(end, text, &s_style_text);
    return end;
}

static lv_obj_t *build_call(lv_obj_t *box)
{
    const sizes_t *sz = s_ui.sz;

    lv_obj_t *toggles = add_box(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(toggles, sz->toggle_h);
    s_ui.mute = add_toggle(toggles, LV_SYMBOL_MUTE, "Mute", mute_cb);
    lv_obj_t *keypad = add_toggle(toggles, LV_SYMBOL_KEYBOARD, "Keypad", keypad_cb);
    s_ui.speaker = add_toggle(toggles, LV_SYMBOL_VOLUME_MAX, "Speaker", speaker_cb);
    set_checked(s_ui.mute, s_ui.st.muted);
    set_checked(s_ui.speaker, s_ui.st.speaker);

    lv_obj_t *volume = add_box(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(volume, sz->volume_h);
    lv_obj_set_flex_align(volume, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *down = add_button(volume, volume_cb, (void *)(intptr_t)-1, false);
    lv_obj_set_size(down, sz->step_w, LV_PCT(100));
    add_label(down, LV_SYMBOL_MINUS, &s_style_text);
    s_ui.volume = add_label(volume, "", &s_style_text);
    lv_obj_add_style(s_ui.volume, &s_style_grow, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.volume, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_t *up = add_button(volume, volume_cb, (void *)(intptr_t)1, false);
    lv_obj_set_size(up, sz->step_w, LV_PCT(100));
    add_label(up, LV_SYMBOL_PLUS, &s_style_text);
    show_volume(s_ui.st.volume);

    lv_obj_t *end = add_end_button(box, LV_SYMBOL_CALL " End call");
    lv_obj_set_size(end, LV_PCT(100), sz->end_h);
    return keypad;
}

static const char *const DTMF_MAP[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    "*", "0", "#", "",
};

/* Twelve keys in one object: a matrix costs the pool far less than twelve
 * buttons with a label each. Tall: the digits sent, the keys, then Hide and
 * End call. Wide: those three in a column beside the keys. */
static lv_obj_t *build_keypad(lv_obj_t *box)
{
    const sizes_t *sz = s_ui.sz;
    lv_obj_t *side = box;
    if (s_ui.wide) {
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
        side = add_box(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_size(side, LV_PCT(38), LV_PCT(100));
        lv_obj_set_flex_align(side, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
    }
    s_ui.digits = add_label(side, s_digits, &s_style_line);
    lv_label_set_long_mode(s_ui.digits, LV_LABEL_LONG_CLIP);

    lv_obj_t *keys = lv_buttonmatrix_create(box);
    lv_obj_remove_style_all(keys);
    lv_obj_add_style(keys, &s_style_keys, LV_PART_MAIN);
    lv_obj_add_style(keys, &s_style_grow, LV_PART_MAIN);
    lv_obj_add_style(keys, &s_style_button, LV_PART_ITEMS);
    lv_obj_add_style(keys, &s_style_symbol, LV_PART_ITEMS);
    lv_obj_add_style(keys, &s_style_pressed, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_style(keys, &s_style_focus, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_buttonmatrix_set_map(keys, DTMF_MAP);
    if (s_ui.wide) {
        lv_obj_set_height(keys, LV_PCT(100));
    } else {
        lv_obj_set_width(keys, LV_PCT(100));
    }
    lv_obj_add_event_cb(keys, dtmf_key_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(keys, key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(s_ui.group, keys);

    lv_obj_t *bottom = side;
    if (!s_ui.wide) {
        bottom = add_box(box, LV_FLEX_FLOW_ROW);
        lv_obj_set_height(bottom, sz->end_h);
    }
    lv_obj_t *hide = add_button(bottom, hide_keypad_cb, NULL, false);
    add_label(hide, "Hide", &s_style_text);
    lv_obj_t *end = add_end_button(bottom, LV_SYMBOL_CALL " End");
    if (s_ui.wide) {
        lv_obj_set_size(hide, LV_PCT(100), sz->small_h);
        lv_obj_set_size(end, LV_PCT(100), sz->end_h);
    } else {
        lv_obj_set_size(hide, sz->hide_w, LV_PCT(100));
        lv_obj_add_style(end, &s_style_grow, LV_PART_MAIN);
        lv_obj_set_height(end, LV_PCT(100));
    }
    return keys;
}

/* The buttons for `mode`, in place of the last mode's. The old ones may be
 * running the event that asked for this, so they are hidden now and deleted
 * after it. */
static void build_controls(screen_mode_t mode)
{
    if (s_ui.controls) {
        lv_obj_add_flag(s_ui.controls, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(s_ui.controls);
    }
    s_ui.controls = NULL;
    s_ui.silence = NULL;
    s_ui.mute = NULL;
    s_ui.speaker = NULL;
    s_ui.volume = NULL;
    s_ui.digits = NULL;
    s_ui.mode = mode;

    /* It takes the room left under the name, its buttons at the bottom. */
    lv_obj_t *box = add_box(s_ui.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(box, &s_style_grow, LV_PART_MAIN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_ui.controls = box;

    lv_obj_t *focus = NULL;
    switch (mode) {
    case MODE_RINGING: focus = build_ringing(box); break;
    case MODE_CALL:    focus = build_call(box); break;
    case MODE_KEYPAD:  focus = build_keypad(box); break;
    default: break;
    }
    if (focus) {
        lv_group_focus_obj(focus);
    }
}

static lv_indev_t *find_keypad(void)
{
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            return indev;
        }
    }
    return NULL;
}

static void open_screen(const call_status_t *st, const char *why)
{
    lv_obj_t *screen = ui_manager_get_screen();
    if (s_ui.root || !screen) {
        return;
    }
    quick_panel_close();
    power_ui_close_menu();
    styles_init();
    s_epaper = display_is_epaper();

    int32_t top = statusbar_get_height();
    int32_t w = lv_obj_get_width(screen);
    int32_t h = lv_obj_get_height(screen) - top;
    s_ui.wide = w > h;
    s_ui.sz = s_ui.wide ? &WIDE : &TALL;

    s_ui.group = lv_group_create();
    s_ui.keypad = find_keypad();
    if (s_ui.keypad) {
        s_ui.keypad_group_before = lv_indev_get_group(s_ui.keypad);
        lv_indev_set_group(s_ui.keypad, s_ui.group);
    }

    /* Opaque and clickable: nothing beneath sees a tap. */
    s_ui.root = lv_obj_create(screen);
    lv_obj_remove_style_all(s_ui.root);
    lv_obj_add_style(s_ui.root, &s_style_root, LV_PART_MAIN);
    lv_obj_set_pos(s_ui.root, 0, top);
    lv_obj_set_size(s_ui.root, w, h);
    lv_obj_add_flag(s_ui.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.status = add_label(s_ui.root, "", &s_style_line);
    s_ui.name = add_label(s_ui.root, "", &s_style_name);
    lv_label_set_long_mode(s_ui.name, LV_LABEL_LONG_DOT);
    s_ui.number = add_label(s_ui.root, "", &s_style_line);
    lv_label_set_long_mode(s_ui.number, LV_LABEL_LONG_DOT);

    s_ui.mode = MODE_NONE;
    s_ui.st = *st;
    apply(st);

    lv_obj_move_foreground(s_ui.root);
    s_open = true;
    ESP_LOGI(TAG, "%s", why);
}

static void delete_group_cb(void *group)
{
    lv_group_delete(group);
}

static void close_screen(const char *why)
{
    if (!s_ui.root) {
        return;
    }
    s_open = false;
    if (s_ui.keypad) {
        lv_indev_set_group(s_ui.keypad, s_ui.keypad_group_before);
    }
    /* Asynchronously: this may be running in the event of one of its own
     * buttons. Its objects leave the group as they are deleted; the group
     * goes once they have. */
    lv_obj_add_flag(s_ui.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(s_ui.root);
    lv_async_call(delete_group_cb, s_ui.group);
    memset(&s_ui, 0, sizeof(s_ui));
    ESP_LOGI(TAG, "%s", why);
}

static void tuck_away(void)
{
    s_tucked = true;
    close_screen("tucked away");
}

static void reopen(void)
{
    call_status_t st;
    get_status(&st);
    s_tucked = false;
    if (is_live(st.state) && !power_ui_sleep_screen_shown()) {
        open_screen(&st, "reopened");
    }
}

/* ------------------------------------------------------------------ */
/* Following the kernel                                                 */
/* ------------------------------------------------------------------ */

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    call_status_t st;
    get_status(&st);

    if (is_live(st.state) && !is_live(s_last_state)) {
        /* A new call: nothing of the last one carries over. */
        s_tucked = false;
        s_silenced = false;
        s_keypad_shown = false;
        s_digits[0] = '\0';
    }
    s_last_state = st.state;

    if (st.state == CALL_IDLE) {
        s_tucked = false;
        close_screen("closed");
        return;
    }

    if (!s_ui.root) {
        if (st.state == CALL_ENDED) {
            /* Ended while tucked away: say so, without the whole screen. */
            if (s_tucked) {
                s_tucked = false;
                char line[40];
                status_line(&st, line, sizeof(line));
                toast_show(line, TOAST_INFO, CALL_ENDED_SHOW_MS);
            }
            return;
        }
        /* A call that rings while the device sleeps wakes it; the screen
         * opens once the sleep screen has gone. */
        if (s_tucked || power_ui_sleep_screen_shown()) {
            return;
        }
        open_screen(&st, st.incoming ? "open incoming" : "open outgoing");
        return;
    }

    if (st.seq != s_ui.st.seq) {
        apply(&st);
    } else if (st.state == CALL_ACTIVE) {
        show_status(&st);   /* the time, rewritten only when its text moves */
    }
}

esp_err_t call_ui_init(void)
{
    s_epaper = display_is_epaper();
    if (!s_timer) {
        s_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
    }
    if (!s_timer) {
        ESP_LOGW(TAG, "no timer; calls will not show");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void call_ui_set_text_font(const lv_font_t *font, call_ui_shape_fn shape)
{
    s_shape = shape;
    s_have_text_font = font != NULL;
    if (!font) return;
    /* A copy, so the font given, which is const, keeps its own fallback. */
    s_text_font = *font;
    if (!s_text_font.fallback) {
        s_text_font.fallback = &lv_font_montserrat_14;
    }
}

bool call_ui_is_open(void)
{
    return s_open;
}

bool call_ui_is_tucked(void)
{
    return s_tucked;
}

bool call_ui_back(void)
{
    if (!s_ui.root) {
        return false;
    }
    call_status_t st;
    get_status(&st);
    if (st.state == CALL_INCOMING) {
        silence();
    } else if (is_live(st.state)) {
        tuck_away();
    } else {
        close_screen("closed");
    }
    return true;
}

bool call_ui_home(void)
{
    if (!s_ui.root) {
        return false;
    }
    call_status_t st;
    get_status(&st);
    if (st.state == CALL_INCOMING) {
        return true;   /* a ringing call stays in front */
    }
    if (is_live(st.state)) {
        tuck_away();
        return true;
    }
    close_screen("closed");
    return false;
}

bool call_ui_chip_text(char *text, size_t size)
{
    call_status_t st;
    get_status(&st);
    if (s_tucked && is_live(st.state)) {
        if (st.state != CALL_ACTIVE) {
            snprintf(text, size, LV_SYMBOL_CALL " ...");
        } else if (s_epaper) {
            /* Whole minutes: the bar is redrawn once a minute, not each
             * second. */
            if (st.duration_s < 60) {
                snprintf(text, size, LV_SYMBOL_CALL " <1m");
            } else {
                snprintf(text, size, LV_SYMBOL_CALL " %um", (unsigned)(st.duration_s / 60));
            }
        } else {
            char time[16];
            format_duration(st.duration_s, time, sizeof(time));
            snprintf(text, size, LV_SYMBOL_CALL " %s", time);
        }
        return true;
    }
    if (st.state != CALL_IDLE) {
        return false;
    }
    uint32_t missed = rs_call_missed_count();
    if (missed != s_missed_logged) {
        ESP_LOGI(TAG, "missed calls: %u", (unsigned)missed);
        s_missed_logged = missed;
    }
    if (missed == 0) {
        return false;
    }
    if (missed > 99) {
        snprintf(text, size, LV_SYMBOL_CALL " 99+");
    } else {
        snprintf(text, size, LV_SYMBOL_CALL " %u", (unsigned)missed);
    }
    return true;
}

void call_ui_chip_tapped(void)
{
    if (s_tucked) {
        reopen();
        return;
    }
    esp_err_t err = app_manager_launch(PHONE_APP_ID);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no Phone app: %s", esp_err_to_name(err));
    }
}

#ifdef SIMULATOR_BUILD
void call_ui_preview(const char *what)
{
    call_status_t st;
    memset(&st, 0, sizeof(st));
    st.state = CALL_ACTIVE;
    st.incoming = true;
    st.speaker = true;
    st.volume = 60;
    st.seq = 1;
    snprintf(st.number, sizeof(st.number), "+4915123456789");
    snprintf(st.name, sizeof(st.name), "Sam Taylor");
    st.duration_s = 83;

    bool keypad = false;
    if (strcmp(what, "incoming") == 0) {
        st.state = CALL_INCOMING;
        st.duration_s = 0;
    } else if (strcmp(what, "calling") == 0) {
        st.state = CALL_DIALING;
        st.incoming = false;
        st.name[0] = '\0';
        st.duration_s = 0;
        snprintf(st.number, sizeof(st.number), "+4930123456");
    } else if (strcmp(what, "keypad") == 0) {
        keypad = true;
        snprintf(s_digits, sizeof(s_digits), "1234#");
    } else if (strcmp(what, "ended") == 0) {
        s_preview_ends = true;
    } else if (strcmp(what, "active") != 0) {
        ESP_LOGW(TAG, "no preview \"%s\"", what);
        return;
    }
    s_preview = st;
    s_preview_since = lv_tick_get();
    s_previewing = true;
    /* Seen as the call already up, so the first tick keeps the keypad. */
    s_last_state = st.state;
    s_keypad_shown = keypad;
    ESP_LOGI(TAG, "preview %s", what);
}
#endif
