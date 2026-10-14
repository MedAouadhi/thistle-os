/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — AI Assistant setup screen
 *
 * One scrolling form over the chat: provider, URL, API key, model and
 * system prompt, saved to ASSISTANT_CONFIG_PATH. Every field is in the
 * default group, so Tab, or Enter in a text field, walks the form, for
 * boards with a keyboard and no touch.
 */
#include "assistant/assistant_setup.h"
#include "assistant/assistant_config.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "ui/theme.h"
#include "ui/toast.h"

static const char *TAG = "assistant_setup";

#define TITLE_BAR_H  30
#define FIELD_H      30
#define FIELD_PAD    4
#define SIDE_PAD     8

static struct {
    lv_obj_t *panel;
    lv_obj_t *provider_btns[2]; /* indexed by assistant_provider_t */
    lv_obj_t *url_ta;
    lv_obj_t *key_ta;
    lv_obj_t *show_label;
    lv_obj_t *model_ta;
    lv_obj_t *prompt_ta;
    assistant_provider_t provider;
    assistant_setup_done_cb_t done;
    /* What the card holds. An empty key field keeps the saved key, but only
     * for the same provider and URL, so a key is never sent to a server it
     * was not typed for. */
    assistant_provider_t saved_provider;
    char saved_url[ASSISTANT_URL_MAX];
    char saved_key[ASSISTANT_KEY_MAX];
} s_setup;

/* ------------------------------------------------------------------ */
/* Widgets                                                              */
/* ------------------------------------------------------------------ */

static void style_focus_ring(lv_obj_t *obj)
{
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_set_style_outline_width(obj, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(obj, tc->text, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(obj, 1, LV_STATE_FOCUS_KEY);
}

static void add_to_group(lv_obj_t *obj)
{
    lv_group_t *group = lv_group_get_default();
    if (group) {
        lv_group_add_obj(group, obj);
    }
}

static void create_section_label(lv_obj_t *parent, const char *text)
{
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, tc->text_secondary, LV_PART_MAIN);
    lv_obj_set_style_pad_top(label, 4, LV_PART_MAIN);
}

/* A plain row the width of the form, its children laid out left to right. */
static lv_obj_t *create_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    return row;
}

/* A bordered button; reads inverted while checked. Returns its label. */
static lv_obj_t *create_button(lv_obj_t *row, const char *text,
                               lv_event_cb_t cb, void *user_data)
{
    const theme_colors_t *tc = theme_get_colors();
    lv_obj_t *btn = lv_obj_create(row);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, FIELD_H - 4);
    lv_obj_set_style_bg_color(btn, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, tc->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, tc->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, tc->text, LV_STATE_CHECKED);
    style_focus_ring(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    add_to_group(btn);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, tc->text, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, tc->bg, LV_STATE_CHECKED);
    lv_obj_center(label);
    return label;
}

static void set_checked(lv_obj_t *btn, bool checked)
{
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (checked) {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
        if (label) lv_obj_add_state(label, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(btn, LV_STATE_CHECKED);
        if (label) lv_obj_remove_state(label, LV_STATE_CHECKED);
    }
}

/* LVGL sends a key's release, and with Enter a click, to whatever has focus
 * by then. When Enter moves focus, the release must not click what it moved
 * to: Enter in the prompt would press Save. */
static void ignore_key_release(void)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
        lv_indev_wait_release(indev);
    }
}

/* Enter moves to the next field, as Tab does, instead of typing a line
 * break; no field here has a use for one. */
static void enter_moves_on_cb(lv_event_t *e)
{
    const char *text = lv_event_get_param(e);
    if (!text || strcmp(text, "\n") != 0) {
        return;
    }
    lv_obj_t *ta = lv_event_get_target(e);
    lv_textarea_set_insert_replace(ta, "");
    lv_group_t *group = lv_obj_get_group(ta);
    if (group) {
        lv_group_focus_next(group);
        ignore_key_release();
    }
}

/* A field that wraps its text and grows to max_lines before it scrolls:
 * a URL or a key is too long to read scrolled sideways on a 240 px panel. */
static lv_obj_t *create_textarea(lv_obj_t *parent, int max_lines,
                                 uint32_t max_length, const char *text)
{
    const theme_colors_t *tc = theme_get_colors();
    const int32_t line_h = lv_font_get_line_height(&lv_font_montserrat_14);
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, false);
    lv_textarea_set_max_length(ta, max_length);
    lv_textarea_set_text(ta, text);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_height(ta, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(ta, max_lines * line_h + 2 * (FIELD_PAD + 2),
                                LV_PART_MAIN);
    lv_obj_add_event_cb(ta, enter_moves_on_cb, LV_EVENT_INSERT, NULL);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, tc->text, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, tc->surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, tc->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(ta, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(ta, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(ta, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ta, FIELD_PAD, LV_PART_MAIN);
    add_to_group(ta);
    return ta;
}

/* ------------------------------------------------------------------ */
/* Behaviour                                                            */
/* ------------------------------------------------------------------ */

static bool keeps_saved_key(void)
{
    return s_setup.saved_key[0] != '\0'
        && s_setup.provider == s_setup.saved_provider
        && strcmp(lv_textarea_get_text(s_setup.url_ta), s_setup.saved_url) == 0;
}

static void update_key_placeholder(void)
{
    const char *hint = keeps_saved_key() ? "Saved; type to replace"
        : s_setup.provider == ASSISTANT_PROVIDER_OPENAI
            ? "Optional for local servers"
            : "sk-ant-...";
    lv_textarea_set_placeholder_text(s_setup.key_ta, hint);
}

static void select_provider(assistant_provider_t provider)
{
    const assistant_provider_t old = s_setup.provider;
    s_setup.provider = provider;
    set_checked(s_setup.provider_btns[ASSISTANT_PROVIDER_ANTHROPIC],
                provider == ASSISTANT_PROVIDER_ANTHROPIC);
    set_checked(s_setup.provider_btns[ASSISTANT_PROVIDER_OPENAI],
                provider == ASSISTANT_PROVIDER_OPENAI);

    /* Swap in the new provider's URL and model, unless they were edited. */
    if (old != provider) {
        const char *url = lv_textarea_get_text(s_setup.url_ta);
        if (url[0] == '\0'
            || strcmp(url, assistant_provider_default_url(old)) == 0) {
            lv_textarea_set_text(s_setup.url_ta,
                                 assistant_provider_default_url(provider));
        }
        const char *model = lv_textarea_get_text(s_setup.model_ta);
        if (model[0] == '\0'
            || strcmp(model, assistant_provider_default_model(old)) == 0) {
            lv_textarea_set_text(s_setup.model_ta,
                                 assistant_provider_default_model(provider));
        }
    }
    update_key_placeholder();
}

static void provider_cb(lv_event_t *e)
{
    select_provider((assistant_provider_t)(uintptr_t)lv_event_get_user_data(e));
}

static void url_changed_cb(lv_event_t *e)
{
    (void)e;
    update_key_placeholder();
}

static void show_key_cb(lv_event_t *e)
{
    (void)e;
    const bool hidden = lv_textarea_get_password_mode(s_setup.key_ta);
    lv_textarea_set_password_mode(s_setup.key_ta, !hidden);
    lv_label_set_text(s_setup.show_label, hidden ? "Hide" : "Show");
}

/* Copy a field without the spaces a phone-style keyboard leaves around it. */
static void copy_trimmed(char *out, size_t out_len, const char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\n') {
        text++;
    }
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t'
                          || text[length - 1] == '\n')) {
        length--;
    }
    if (length >= out_len) {
        length = out_len - 1;
    }
    memcpy(out, text, length);
    out[length] = '\0';
}

static void finish(bool saved)
{
    assistant_setup_done_cb_t done = s_setup.done;
    s_setup.done = NULL;
    /* done moves focus off the form first, so deleting the focused field
     * does not hand focus to whatever the group holds next. */
    if (done) {
        done(saved);
    }
    assistant_setup_close();
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    assistant_config_t *config = calloc(1, sizeof(*config));
    if (!config) {
        toast_warn("Out of memory");
        return;
    }
    config->provider = s_setup.provider;
    copy_trimmed(config->api_url, sizeof(config->api_url),
                 lv_textarea_get_text(s_setup.url_ta));
    copy_trimmed(config->api_key, sizeof(config->api_key),
                 lv_textarea_get_text(s_setup.key_ta));
    if (config->api_key[0] == '\0' && keeps_saved_key()) {
        memcpy(config->api_key, s_setup.saved_key, sizeof(config->api_key));
    }
    copy_trimmed(config->model, sizeof(config->model),
                 lv_textarea_get_text(s_setup.model_ta));
    copy_trimmed(config->system_prompt, sizeof(config->system_prompt),
                 lv_textarea_get_text(s_setup.prompt_ta));

    const char *problem = assistant_config_problem(config);
    bool saved = false;
    if (problem) {
        toast_warn(problem);
    } else if (!assistant_config_save(ASSISTANT_CONFIG_PATH, config)) {
        ESP_LOGW(TAG, "Cannot write %s", ASSISTANT_CONFIG_PATH);
        toast_warn("Could not save to the SD card");
    } else {
        ESP_LOGI(TAG, "Saved %s provider config", assistant_provider_id(config->provider));
        toast_info("Saved");
        saved = true;
    }
    memset(config, 0, sizeof(*config));
    free(config);

    if (saved) {
        finish(true);
    }
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    finish(false);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void assistant_setup_open(lv_obj_t *parent, assistant_setup_done_cb_t done)
{
    if (s_setup.panel) {
        return;
    }
    assistant_config_t *config = malloc(sizeof(*config));
    if (!config) {
        toast_warn("Out of memory");
        return;
    }
    const bool has_file = assistant_config_load(ASSISTANT_CONFIG_PATH, config);

    memset(&s_setup, 0, sizeof(s_setup));
    s_setup.done = done;
    s_setup.provider = config->provider;
    if (has_file) {
        s_setup.saved_provider = config->provider;
        memcpy(s_setup.saved_url, config->api_url, sizeof(s_setup.saved_url));
        memcpy(s_setup.saved_key, config->api_key, sizeof(s_setup.saved_key));
    }

    const theme_colors_t *tc = theme_get_colors();
    lv_obj_t *panel = lv_obj_create(parent);
    s_setup.panel = panel;
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    /* Title bar; tapping it goes back without saving. */
    lv_obj_t *title_bar = lv_obj_create(panel);
    lv_obj_set_size(title_bar, LV_PCT(100), TITLE_BAR_H);
    lv_obj_set_style_bg_color(title_bar, tc->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_color(title_bar, tc->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(title_bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(title_bar, tc->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(title_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(title_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(title_bar, SIDE_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(title_bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(title_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(title_bar, cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(title_bar);
    lv_label_set_text(title, "< Assistant setup");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, tc->text, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    /* The form, scrolling under the title bar. */
    lv_obj_t *form = lv_obj_create(panel);
    lv_obj_set_width(form, LV_PCT(100));
    lv_obj_set_flex_grow(form, 1);
    lv_obj_set_style_bg_opa(form, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(form, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(form, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(form, SIDE_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(form, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(form, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(form, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(form, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(form, tc->text, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(form, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(form, 2, LV_PART_SCROLLBAR);

    create_section_label(form, "Provider");
    lv_obj_t *provider_row = create_row(form);
    create_button(provider_row, "Anthropic", provider_cb,
                  (void *)(uintptr_t)ASSISTANT_PROVIDER_ANTHROPIC);
    create_button(provider_row, "OpenAI style", provider_cb,
                  (void *)(uintptr_t)ASSISTANT_PROVIDER_OPENAI);
    s_setup.provider_btns[ASSISTANT_PROVIDER_ANTHROPIC] =
        lv_obj_get_child(provider_row, 0);
    s_setup.provider_btns[ASSISTANT_PROVIDER_OPENAI] =
        lv_obj_get_child(provider_row, 1);

    create_section_label(form, "URL");
    s_setup.url_ta = create_textarea(form, 3, ASSISTANT_URL_MAX - 1,
                                     config->api_url);
    lv_obj_add_event_cb(s_setup.url_ta, url_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    create_section_label(form, "API key");
    lv_obj_t *key_row = create_row(form);
    lv_obj_set_style_pad_hor(key_row, 0, LV_PART_MAIN);
    s_setup.key_ta = create_textarea(key_row, 3, ASSISTANT_KEY_MAX - 1, "");
    lv_obj_set_width(s_setup.key_ta, 0);
    lv_obj_set_flex_grow(s_setup.key_ta, 1);
    lv_textarea_set_password_mode(s_setup.key_ta, true);
    s_setup.show_label = create_button(key_row, "Show", show_key_cb, NULL);

    create_section_label(form, "Model");
    s_setup.model_ta = create_textarea(form, 2, ASSISTANT_MODEL_MAX - 1,
                                       config->model);
    lv_textarea_set_placeholder_text(s_setup.model_ta, "Model name");

    create_section_label(form, "System prompt");
    s_setup.prompt_ta = create_textarea(form, 4, ASSISTANT_PROMPT_MAX - 1,
                                        config->system_prompt);

    lv_obj_t *actions = create_row(form);
    lv_obj_set_style_pad_top(actions, 8, LV_PART_MAIN);
    create_button(actions, "Save", save_cb, NULL);
    create_button(actions, "Cancel", cancel_cb, NULL);

    memset(config, 0, sizeof(*config));
    free(config);

    ESP_LOGI(TAG, "Setup open");
    select_provider(s_setup.provider);
    lv_group_focus_obj(s_setup.provider_btns[s_setup.provider]);
    ignore_key_release(); /* opened by Enter in the chat */
    lv_obj_scroll_to_y(form, 0, LV_ANIM_OFF);
}

void assistant_setup_close(void)
{
    if (s_setup.panel) {
        lv_obj_delete(s_setup.panel);
    }
    memset(&s_setup, 0, sizeof(s_setup));
}

void assistant_setup_cancel(void)
{
    finish(false);
}

bool assistant_setup_is_open(void)
{
    return s_setup.panel != NULL;
}
