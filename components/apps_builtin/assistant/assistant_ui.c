/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — AI Assistant chat UI
 *
 * A single-conversation chat with an AI over WiFi or LTE, through either
 * Anthropic's Messages API or an OpenAI-style Chat Completions server. The
 * gear in the header opens the setup form (assistant_setup.c), which
 * writes THISTLE_SDCARD/config/assistant.json; see assistant_config.h for
 * its fields.
 *
 * A request runs on a task of its own, so the UI keeps drawing and taking
 * keys while the server thinks; an lv_timer picks the outcome up. Replies,
 * and notes such as API errors, land in the chat. Notes are never sent back
 * to the model as context.
 *
 * The conversation is kept in THISTLE_SDCARD/data/assistant/
 * last_conversation.txt, written after every reply and when the app pauses.
 */
#include "assistant/assistant_app.h"
#include "assistant/assistant_config.h"
#include "assistant/assistant_request.h"
#include "assistant/assistant_setup.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lvgl.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal/sdcard_path.h"
#include "thistle/app_manager.h"
#include "thistle/power_manager.h"
#include "thistle/net_manager.h"
#include "thistle/wifi_manager.h"
#include "thistle/time_manager.h"
#include "ui/manager.h"
#include "ui/theme.h"
#include "ui/toast.h"

static const char *TAG = "assistant_ui";

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define MAX_MESSAGES        30
/* Bytes kept of any one message; about what a 512-token reply runs to. */
#define MAX_MSG_TEXT        2048
/* Answered questions sent along with a new one, each with its answer. */
#define HISTORY_EXCHANGES   3
#define CONV_DATA_DIR       THISTLE_SDCARD "/data/assistant"
#define CONV_FILE_PATH      THISTLE_SDCARD "/data/assistant/last_conversation.txt"
/* A saved line: sender, time, and the text with every byte escaped. */
#define CONV_LINE_MAX       (2 * MAX_MSG_TEXT + 32)

/* A whole response body is kept, to be parsed once it is complete. A
 * 512-token Anthropic reply is about 3 KB. */
#define RESPONSE_MAX        (16 * 1024)
#define REQUEST_TIMEOUT_MS  60000
/* The chat stops waiting a while after the HTTP client should have. */
#define REQUEST_GIVE_UP_US  (90LL * 1000 * 1000)
/* TLS with certificate-bundle verification needs about this much. */
#define REQUEST_STACK       8192
#define REQUEST_PRIORITY    5
#define POLL_MS             250

static int s_app_w = 240;
static int s_app_h = 296;
#define HEADER_H     30
#define INPUT_BAR_H  40
static int s_msg_list_h = 226; /* s_app_h - HEADER_H - INPUT_BAR_H */

/* ------------------------------------------------------------------ */
/* Data types                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    assistant_turn_kind_t kind;
    char time_str[12];      /* "14:05", or "2:05 PM" */
    char *text;             /* owned; PSRAM where the board has it */
} ai_message_t;

typedef enum {
    JOB_RUNNING,
    JOB_DONE,       /* the outcome is in kind and text */
    JOB_ABANDONED,  /* the chat stopped waiting; the task frees the job */
} job_state_t;

/* One request, shared by the chat and the task that runs it. Whichever of
 * the two lets go last frees it, decided by one compare-and-swap on state.
 * It holds the API key, so it stays in internal RAM. */
typedef struct {
    atomic_int state;
    int64_t started_us;
    assistant_provider_t provider;
    char url[ASSISTANT_URL_MAX];
    char api_key[ASSISTANT_KEY_MAX];
    char *body;
    size_t body_len;
    char *response;         /* RESPONSE_MAX bytes, PSRAM where possible */
    size_t response_len;
    bool response_overflow;
    assistant_turn_kind_t kind;
    char text[MAX_MSG_TEXT];
} assistant_job_t;

/* ------------------------------------------------------------------ */
/* Module state                                                         */
/* ------------------------------------------------------------------ */

/* Only the UI task touches this; the request task sees only its job. */
static EXT_RAM_BSS_ATTR struct {
    lv_obj_t *root;
    lv_obj_t *chat;
    lv_obj_t *msg_list;
    lv_obj_t *hint;
    lv_obj_t *input_ta;
    lv_obj_t *gear_btn;
    lv_obj_t *status_label;
    ai_message_t messages[MAX_MESSAGES];
    int msg_count;
    assistant_job_t *job;
    lv_timer_t *poll_timer;
} s_ai;

/* ------------------------------------------------------------------ */
/* Memory                                                               */
/* ------------------------------------------------------------------ */

static void *psram_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(size);
}

/* A copy of text, cut to MAX_MSG_TEXT at a character boundary. */
static char *text_dup(const char *text)
{
    size_t length = strlen(text);
    if (length > MAX_MSG_TEXT - 1) {
        length = MAX_MSG_TEXT - 1;
        while (length > 0 && ((unsigned char)text[length] & 0xC0U) == 0x80U) {
            length--;
        }
    }
    char *copy = psram_alloc(length + 1);
    if (copy) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

static void job_free(assistant_job_t *job)
{
    free(job->body);
    free(job->response);
    memset(job->api_key, 0, sizeof(job->api_key));
    free(job);
}

/* ------------------------------------------------------------------ */
/* Messages                                                             */
/* ------------------------------------------------------------------ */

static const char *sender_name(assistant_turn_kind_t kind)
{
    switch (kind) {
        case ASSISTANT_TURN_QUESTION: return "You";
        case ASSISTANT_TURN_NOTE: return "Note";
        default: return "AI";
    }
}

static assistant_turn_kind_t sender_kind(const char *sender)
{
    if (strcmp(sender, "You") == 0) return ASSISTANT_TURN_QUESTION;
    if (strcmp(sender, "Note") == 0) return ASSISTANT_TURN_NOTE;
    return ASSISTANT_TURN_REPLY;
}

static void create_message_bubble(const ai_message_t *msg)
{
    const theme_colors_t *colors = theme_get_colors();
    const bool is_user = msg->kind == ASSISTANT_TURN_QUESTION;
    const bool is_note = msg->kind == ASSISTANT_TURN_NOTE;

    lv_obj_t *bubble = lv_obj_create(s_ai.msg_list);
    lv_obj_set_width(bubble, LV_PCT(90));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(bubble, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(bubble, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(bubble, colors->text_secondary, LV_PART_MAIN);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    if (is_user) {
        /* User messages: right-aligned, primary/accent background */
        lv_obj_set_style_bg_color(bubble, colors->primary, LV_PART_MAIN);
        lv_obj_set_align(bubble, LV_ALIGN_RIGHT_MID);
    } else if (is_note) {
        /* Notes: plain background, so they read as asides */
        lv_obj_set_style_bg_color(bubble, colors->bg, LV_PART_MAIN);
    } else {
        /* AI messages: left-aligned, surface background */
        lv_obj_set_style_bg_color(bubble, colors->surface, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bubble, 2, LV_PART_MAIN);

    /* Header: sender + timestamp */
    lv_obj_t *header = lv_label_create(bubble);
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "[%s] %s", sender_name(msg->kind), msg->time_str);
    lv_label_set_text(header, hdr);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(header,
        is_user ? lv_color_white() : colors->text_secondary,
        LV_PART_MAIN);

    /* Message body — word-wrap for long AI responses */
    lv_obj_t *body = lv_label_create(bubble);
    lv_label_set_text(body, msg->text);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_text_font(body, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(body,
        is_user ? lv_color_white()
                : is_note ? colors->text_secondary : colors->text,
        LV_PART_MAIN);
}

/* Add a message, dropping the oldest when the chat is full. */
static void append_message(assistant_turn_kind_t kind, const char *time_str,
                           const char *text)
{
    char *copy = text_dup(text);
    if (!copy) {
        ESP_LOGW(TAG, "No memory for a message");
        return;
    }

    if (s_ai.hint) {
        lv_obj_delete(s_ai.hint);
        s_ai.hint = NULL;
    }
    if (s_ai.msg_count == MAX_MESSAGES) {
        free(s_ai.messages[0].text);
        memmove(&s_ai.messages[0], &s_ai.messages[1],
                (MAX_MESSAGES - 1) * sizeof(s_ai.messages[0]));
        s_ai.msg_count--;
        /* Bubbles match messages one for one once the hint is gone. */
        if (s_ai.msg_list && lv_obj_get_child_count(s_ai.msg_list) > 0) {
            lv_obj_delete(lv_obj_get_child(s_ai.msg_list, 0));
        }
    }

    ai_message_t *msg = &s_ai.messages[s_ai.msg_count++];
    msg->kind = kind;
    snprintf(msg->time_str, sizeof(msg->time_str), "%s", time_str);
    msg->text = copy;

    if (s_ai.msg_list) {
        create_message_bubble(msg);
    }
}

static void add_message(assistant_turn_kind_t kind, const char *text)
{
    char time_buf[12];
    time_manager_format_time(time_buf, sizeof(time_buf));
    append_message(kind, time_buf, text);
    if (s_ai.msg_list) {
        lv_obj_scroll_to_y(s_ai.msg_list, LV_COORD_MAX, LV_ANIM_ON);
    }
}

static void free_messages(void)
{
    for (int i = 0; i < s_ai.msg_count; i++) {
        free(s_ai.messages[i].text);
        s_ai.messages[i].text = NULL;
    }
    s_ai.msg_count = 0;
}

/* ------------------------------------------------------------------ */
/* Persistence                                                          */
/* ------------------------------------------------------------------ */

static void ensure_data_dir(void)
{
    struct stat st;
    if (stat(CONV_DATA_DIR, &st) == 0) {
        return;
    }
    mkdir(THISTLE_SDCARD "/data", 0755);
    if (mkdir(CONV_DATA_DIR, 0755) != 0) {
        ESP_LOGW(TAG, "mkdir %s failed", CONV_DATA_DIR);
    }
}

/*
 * Save the conversation to the SD card, one message per line:
 * "sender|time|text", with backslashes and newlines in the text escaped so
 * that a line is always a whole message.
 */
esp_err_t assistant_ui_save_conversation(void)
{
    ensure_data_dir();

    FILE *f = fopen(CONV_FILE_PATH, "w");
    if (!f) {
        ESP_LOGW(TAG, "Cannot open %s for write", CONV_FILE_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    for (int i = 0; i < s_ai.msg_count; i++) {
        const ai_message_t *m = &s_ai.messages[i];
        fprintf(f, "%s|%s|", sender_name(m->kind), m->time_str);
        for (const char *c = m->text; *c; c++) {
            if (*c == '\\') {
                fputs("\\\\", f);
            } else if (*c == '\n') {
                fputs("\\n", f);
            } else if (*c != '\r') {
                fputc(*c, f);
            }
        }
        fputc('\n', f);
    }

    fclose(f);
    ESP_LOGI(TAG, "Conversation saved (%d messages)", s_ai.msg_count);
    return ESP_OK;
}

/* Undo the escaping in place. Files from before it existed have none. */
static void unescape(char *text)
{
    char *out = text;
    for (const char *in = text; *in; in++) {
        if (in[0] == '\\' && in[1] == 'n') {
            *out++ = '\n';
            in++;
        } else if (in[0] == '\\' && in[1] == '\\') {
            *out++ = '\\';
            in++;
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
}

static void load_conversation(void)
{
    FILE *f = fopen(CONV_FILE_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No saved conversation at %s", CONV_FILE_PATH);
        return;
    }
    char *line = psram_alloc(CONV_LINE_MAX);
    if (!line) {
        fclose(f);
        return;
    }

    while (fgets(line, CONV_LINE_MAX, f) != NULL) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        /* Parse: sender|time_str|text */
        char *p1 = strchr(line, '|');
        if (!p1) continue;
        *p1 = '\0';
        char *p2 = strchr(p1 + 1, '|');
        if (!p2) continue;
        *p2 = '\0';

        unescape(p2 + 1);
        append_message(sender_kind(line), p1 + 1, p2 + 1);
    }

    free(line);
    fclose(f);
    ESP_LOGI(TAG, "Loaded %d messages from conversation history", s_ai.msg_count);

    if (s_ai.msg_list && s_ai.msg_count > 0) {
        lv_obj_scroll_to_y(s_ai.msg_list, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

/* ------------------------------------------------------------------ */
/* Status                                                               */
/* ------------------------------------------------------------------ */

static void update_status(void)
{
    if (!s_ai.status_label) return;
    const theme_colors_t *colors = theme_get_colors();
    const bool connected = net_is_connected();
    lv_label_set_text(s_ai.status_label,
                      s_ai.job ? "Thinking..." : connected ? "Connected" : "Offline");
    lv_obj_set_style_text_color(s_ai.status_label,
                                connected ? colors->primary : colors->text_secondary,
                                LV_PART_MAIN);
}

/* ------------------------------------------------------------------ */
/* The request task                                                     */
/* ------------------------------------------------------------------ */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    assistant_job_t *job = (assistant_job_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && job && evt->data_len > 0) {
        size_t length = (size_t)evt->data_len;
        if (length > RESPONSE_MAX - job->response_len) {
            length = RESPONSE_MAX - job->response_len;
            job->response_overflow = true;
        }
        memcpy(job->response + job->response_len, evt->data, length);
        job->response_len += length;
    }
    return ESP_OK;
}

static void set_note(assistant_job_t *job, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(job->text, sizeof(job->text), format, args);
    va_end(args);
    job->kind = ASSISTANT_TURN_NOTE;
}

static void run_request(assistant_job_t *job)
{
    esp_http_client_config_t http_config = {
        .url                   = job->url,
        .timeout_ms            = REQUEST_TIMEOUT_MS,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .event_handler         = http_event_handler,
        .user_data             = job,
        /* A redirected POST would land its body after the first one's. */
        .disable_auto_redirect = true,
        /* Room for a request line and an Authorization header with a long
         * OpenAI project key. */
        .buffer_size_tx        = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        set_note(job, "Could not use the URL %s. Check it in Setup.", job->url);
        return;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "content-type", "application/json");
    if (job->provider == ASSISTANT_PROVIDER_ANTHROPIC) {
        esp_http_client_set_header(client, "x-api-key", job->api_key);
        esp_http_client_set_header(client, "anthropic-version", "2023-06-01");
    } else if (job->api_key[0] != '\0') {
        char auth[ASSISTANT_KEY_MAX + 8];
        snprintf(auth, sizeof(auth), "Bearer %s", job->api_key);
        esp_http_client_set_header(client, "authorization", auth);
        memset(auth, 0, sizeof(auth));
    }

    esp_err_t err = esp_http_client_set_post_field(client, job->body,
                                                   (int)job->body_len);
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
        set_note(job, "Could not reach the server (%s). Check the connection "
                 "and the URL.", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "HTTP %d, %u bytes", status, (unsigned)job->response_len);
    if (job->response_overflow) {
        set_note(job, "The reply was too long for this device.");
        return;
    }

    const bool http_ok = status >= 200 && status < 300;
    const assistant_reply_status_t parsed = assistant_response_parse(
        job->provider, job->response, job->response_len,
        job->text, sizeof(job->text));
    if (http_ok && parsed == ASSISTANT_REPLY_OK) {
        if (job->text[0] != '\0') {
            job->kind = ASSISTANT_TURN_REPLY;
        } else {
            set_note(job, "The reply was empty.");
        }
    } else if (parsed == ASSISTANT_REPLY_API_ERROR) {
        /* Put the status in front of the API's own message, in place. */
        char prefix[40];
        const size_t prefix_len = (size_t)snprintf(
            prefix, sizeof(prefix), "API error (HTTP %d): ", status);
        size_t message_len = strlen(job->text);
        if (message_len > sizeof(job->text) - 1 - prefix_len) {
            message_len = sizeof(job->text) - 1 - prefix_len;
            while (message_len > 0
                   && ((unsigned char)job->text[message_len] & 0xC0U) == 0x80U) {
                message_len--;
            }
        }
        memmove(job->text + prefix_len, job->text, message_len);
        memcpy(job->text, prefix, prefix_len);
        job->text[prefix_len + message_len] = '\0';
        job->kind = ASSISTANT_TURN_NOTE;
    } else if (http_ok) {
        set_note(job, "The reply could not be read.");
    } else {
        set_note(job, "The server answered HTTP %d.", status);
    }
}

static void request_task(void *arg)
{
    assistant_job_t *job = (assistant_job_t *)arg;
    /* An answer can take a minute; sleep would drop the connection. */
    uint32_t awake = power_manager_hold("the assistant is answering");
    run_request(job);
    power_manager_release(awake);
    free(job->body);
    job->body = NULL;
    free(job->response);
    job->response = NULL;

    int expected = JOB_RUNNING;
    if (!atomic_compare_exchange_strong(&job->state, &expected, JOB_DONE)) {
        job_free(job); /* the chat stopped waiting */
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Sending                                                              */
/* ------------------------------------------------------------------ */

static void poll_cb(lv_timer_t *timer)
{
    assistant_job_t *job = s_ai.job;
    if (job && atomic_load(&job->state) == JOB_DONE) {
        s_ai.job = NULL;
        add_message(job->kind, job->text);
        job_free(job);
    } else if (job && esp_timer_get_time() - job->started_us > REQUEST_GIVE_UP_US) {
        int expected = JOB_RUNNING;
        if (!atomic_compare_exchange_strong(&job->state, &expected,
                                            JOB_ABANDONED)) {
            return; /* it finished just now; the next tick takes it */
        }
        s_ai.job = NULL;
        add_message(ASSISTANT_TURN_NOTE, "No reply came in time.");
    } else if (job) {
        return;
    }

    lv_timer_delete(timer);
    s_ai.poll_timer = NULL;
    update_status();
    assistant_ui_save_conversation();
}

/* Start a request for body, which the job then owns. */
static bool start_request(const assistant_config_t *config, char *body,
                          size_t body_len)
{
    assistant_job_t *job = calloc(1, sizeof(*job));
    char *response = psram_alloc(RESPONSE_MAX);
    if (!job || !response) {
        free(job);
        free(response);
        return false;
    }
    atomic_init(&job->state, JOB_RUNNING);
    job->started_us = esp_timer_get_time();
    job->provider = config->provider;
    memcpy(job->url, config->api_url, sizeof(job->url));
    memcpy(job->api_key, config->api_key, sizeof(job->api_key));
    job->body = body;
    job->body_len = body_len;
    job->response = response;

    if (xTaskCreate(request_task, "assistant", REQUEST_STACK, job,
                    REQUEST_PRIORITY, NULL) != pdPASS) {
        job->body = NULL; /* still the caller's */
        job_free(job);
        return false;
    }
    s_ai.job = job;
    s_ai.poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
    return true;
}

static void open_setup(void);

/* Send the user's message. Returns false, leaving it in the input field,
 * if it was not taken. */
static bool send_to_ai(const char *user_msg)
{
    if (!user_msg || user_msg[0] == '\0') return false;

    if (s_ai.job) {
        toast_info("Still waiting for the last reply");
        return false;
    }

    assistant_config_t *config = malloc(sizeof(*config));
    if (!config) {
        toast_warn("Out of memory");
        return false;
    }
    assistant_config_load(ASSISTANT_CONFIG_PATH, config);

    bool taken = false;
    const char *problem = assistant_config_problem(config);
    if (problem) {
        toast_warn(problem);
        open_setup();
    } else if (!net_is_connected()) {
        toast_warn("No network. Connect to WiFi or turn on mobile data.");
        update_status();
    } else {
        /* Chosen before the question joins the chat, which can drop the
         * oldest message and with it text the context points into. */
        assistant_turn_t turns[MAX_MESSAGES];
        for (int i = 0; i < s_ai.msg_count; i++) {
            turns[i].kind = s_ai.messages[i].kind;
            turns[i].text = s_ai.messages[i].text;
        }
        assistant_request_message_t history[2 * HISTORY_EXCHANGES];
        const size_t history_count = assistant_history_select(
            turns, (size_t)s_ai.msg_count, HISTORY_EXCHANGES, history);

        size_t body_len = 0;
        char *body = assistant_request_build(
            config->provider, config->model, config->system_prompt,
            history, history_count, user_msg, &body_len);
        if (!body) {
            toast_warn("Out of memory");
        } else {
            taken = true;
            add_message(ASSISTANT_TURN_QUESTION, user_msg);
            if (!start_request(config, body, body_len)) {
                free(body);
                add_message(ASSISTANT_TURN_NOTE,
                            "Could not start the request: out of memory.");
            }
            update_status();
        }
    }

    memset(config, 0, sizeof(*config));
    free(config);
    return taken;
}

/* ------------------------------------------------------------------ */
/* Setup                                                                */
/* ------------------------------------------------------------------ */

static void focus_input(void)
{
    lv_group_t *grp = lv_group_get_default();
    if (grp && s_ai.input_ta) {
        lv_group_focus_obj(s_ai.input_ta);
    }
}

static void set_hint(void)
{
    if (!s_ai.hint) return;
    assistant_config_t *config = malloc(sizeof(*config));
    if (!config) return;
    assistant_config_load(ASSISTANT_CONFIG_PATH, config);
    lv_label_set_text(s_ai.hint, assistant_config_problem(config)
        ? "Choose a provider and add an API key: tap " LV_SYMBOL_SETTINGS
          " above."
        : "Ask anything.");
    memset(config, 0, sizeof(*config));
    free(config);
}

static void setup_done(bool saved)
{
    lv_obj_remove_flag(s_ai.chat, LV_OBJ_FLAG_HIDDEN);
    focus_input();
    if (saved) {
        set_hint();
    }
}

static void open_setup(void)
{
    if (!s_ai.root || assistant_setup_is_open()) return;
    lv_obj_add_flag(s_ai.chat, LV_OBJ_FLAG_HIDDEN);
    assistant_setup_open(s_ai.root, setup_done);
    if (!assistant_setup_is_open()) {
        lv_obj_remove_flag(s_ai.chat, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The Back key: from the setup form to the chat, without saving. */
static bool assistant_back(void)
{
    if (!assistant_setup_is_open()) return false;
    assistant_setup_cancel();
    return true;
}

/* ------------------------------------------------------------------ */
/* Event callbacks                                                      */
/* ------------------------------------------------------------------ */

static void send_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!s_ai.input_ta) return;

    const char *text = lv_textarea_get_text(s_ai.input_ta);
    if (!text || text[0] == '\0') return;

    /* Copy, since sending may rebuild the screen around the field. */
    char *msg = text_dup(text);
    if (!msg) return;
    if (send_to_ai(msg) && s_ai.input_ta) {
        lv_textarea_set_text(s_ai.input_ta, "");
    }
    free(msg);
}

static void ta_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ENTER) {
        send_btn_cb(e);
    } else if (key == LV_KEY_ESC) {
        app_manager_launch("com.thistle.launcher");
    }
}

static void root_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC) {
        app_manager_launch("com.thistle.launcher");
    }
}

static void gear_cb(lv_event_t *e)
{
    (void)e;
    open_setup();
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t assistant_ui_create(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "Creating assistant UI");

    if (parent == NULL) {
        parent = lv_scr_act();
    }

    memset(&s_ai, 0, sizeof(s_ai));

    lv_obj_update_layout(parent);
    s_app_w = lv_obj_get_width(parent);
    s_app_h = lv_obj_get_height(parent);
    if (s_app_w == 0) s_app_w = 240;
    if (s_app_h == 0) s_app_h = 296;
    s_msg_list_h = s_app_h - HEADER_H - INPUT_BAR_H;

    const theme_colors_t *colors = theme_get_colors();

    /* ----------------------------------------------------------------
     * Root container; holds the chat, and the setup form when it is open
     * ---------------------------------------------------------------- */
    s_ai.root = lv_obj_create(parent);
    lv_obj_set_size(s_ai.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_ai.root, 0, 0);
    lv_obj_set_style_bg_color(s_ai.root, colors->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ai.root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ai.root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ai.root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ai.root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_ai.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ai.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ai.root, root_key_cb, LV_EVENT_KEY, NULL);

    /* Hidden while setup is open, which also keeps Tab out of it. */
    s_ai.chat = lv_obj_create(s_ai.root);
    lv_obj_set_size(s_ai.chat, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_ai.chat, 0, 0);
    lv_obj_set_style_bg_opa(s_ai.chat, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ai.chat, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ai.chat, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ai.chat, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_ai.chat, LV_OBJ_FLAG_SCROLLABLE);

    /* ----------------------------------------------------------------
     * Header bar (30px)
     * Contains: "Assistant" title, connection status, setup gear
     * ---------------------------------------------------------------- */
    lv_obj_t *header = lv_obj_create(s_ai.chat);
    lv_obj_set_size(header, s_app_w, HEADER_H);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, colors->surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(header, colors->text_secondary, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_left(header, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(header, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_top(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(header, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title_label = lv_label_create(header);
    lv_label_set_text(title_label, "Assistant");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(title_label, colors->text, LV_PART_MAIN);
    lv_obj_set_flex_grow(title_label, 1);

    s_ai.status_label = lv_label_create(header);
    lv_obj_set_style_text_font(s_ai.status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    update_status();

    s_ai.gear_btn = lv_obj_create(header);
    lv_obj_set_size(s_ai.gear_btn, 28, HEADER_H - 6);
    lv_obj_set_style_bg_opa(s_ai.gear_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ai.gear_btn, colors->primary, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_ai.gear_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_ai.gear_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ai.gear_btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ai.gear_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_ai.gear_btn, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(s_ai.gear_btn, colors->text, LV_STATE_FOCUS_KEY);
    lv_obj_clear_flag(s_ai.gear_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ai.gear_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ai.gear_btn, gear_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *gear_label = lv_label_create(s_ai.gear_btn);
    lv_label_set_text(gear_label, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(gear_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(gear_label, colors->text, LV_PART_MAIN);
    lv_obj_center(gear_label);

    /* ----------------------------------------------------------------
     * Message list (scrollable, between header and input bar)
     * ---------------------------------------------------------------- */
    s_ai.msg_list = lv_obj_create(s_ai.chat);
    lv_obj_set_size(s_ai.msg_list, s_app_w, s_msg_list_h);
    lv_obj_set_pos(s_ai.msg_list, 0, HEADER_H);
    lv_obj_set_style_bg_color(s_ai.msg_list, colors->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ai.msg_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ai.msg_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ai.msg_list, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ai.msg_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_ai.msg_list, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ai.msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_ai.msg_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ai.msg_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_ai.msg_list, colors->primary, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_ai.msg_list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_ai.msg_list, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_ai.msg_list, 0, LV_PART_SCROLLBAR);

    /* Shown until the first message arrives. */
    s_ai.hint = lv_label_create(s_ai.msg_list);
    lv_label_set_long_mode(s_ai.hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ai.hint, LV_PCT(100));
    lv_obj_set_style_text_font(s_ai.hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ai.hint, colors->text_secondary, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ai.hint, 4, LV_PART_MAIN);
    set_hint();

    /* ----------------------------------------------------------------
     * Input bar (40px at bottom)
     * [Ask anything...          ] [>]
     * ---------------------------------------------------------------- */
    lv_obj_t *input_bar = lv_obj_create(s_ai.chat);
    lv_obj_set_size(input_bar, s_app_w, INPUT_BAR_H);
    lv_obj_align(input_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(input_bar, colors->surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(input_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(input_bar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_color(input_bar, colors->text_secondary, LV_PART_MAIN);
    lv_obj_set_style_border_width(input_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(input_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(input_bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(input_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(input_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_bar,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(input_bar, 4, LV_PART_MAIN);

    /* Text area — grows to fill available width */
    s_ai.input_ta = lv_textarea_create(input_bar);
    lv_textarea_set_one_line(s_ai.input_ta, true);
    lv_textarea_set_max_length(s_ai.input_ta, MAX_MSG_TEXT - 1);
    lv_textarea_set_placeholder_text(s_ai.input_ta, "Ask anything...");
    lv_obj_set_flex_grow(s_ai.input_ta, 1);
    lv_obj_set_height(s_ai.input_ta, 30);
    lv_obj_set_style_text_font(s_ai.input_ta, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_add_event_cb(s_ai.input_ta, ta_key_cb, LV_EVENT_KEY, NULL);

    /* Focus textarea so physical keyboard types into it immediately; Tab
     * reaches the gear. */
    lv_group_t *grp = lv_group_get_default();
    if (grp) {
        lv_group_add_obj(grp, s_ai.input_ta);
        lv_group_add_obj(grp, s_ai.gear_btn);
        lv_group_focus_obj(s_ai.input_ta);
    }

    /* Send button (">") */
    lv_obj_t *send_btn = lv_button_create(input_bar);
    lv_obj_set_size(send_btn, 40, 30);
    lv_obj_set_style_bg_color(send_btn, colors->primary, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(send_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(send_btn, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(send_btn, send_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *send_lbl = lv_label_create(send_btn);
    lv_label_set_text(send_lbl, ">");
    lv_obj_set_style_text_font(send_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(send_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(send_lbl);

    /* ----------------------------------------------------------------
     * Load persisted conversation from SD card
     * ---------------------------------------------------------------- */
    load_conversation();

    ui_manager_set_back_handler(s_ai.root, assistant_back);

    return ESP_OK;
}

void assistant_ui_show(void)
{
    if (s_ai.root) {
        lv_obj_clear_flag(s_ai.root, LV_OBJ_FLAG_HIDDEN);

        /* Re-focus textarea so keyboard input goes here; the setup form
         * keeps its own focus. */
        if (!assistant_setup_is_open()) {
            focus_input();
        }
        update_status();
    }
}

void assistant_ui_hide(void)
{
    if (s_ai.root) {
        lv_obj_add_flag(s_ai.root, LV_OBJ_FLAG_HIDDEN);
    }
}

void assistant_ui_destroy(void)
{
    if (s_ai.poll_timer) {
        lv_timer_delete(s_ai.poll_timer);
        s_ai.poll_timer = NULL;
    }
    assistant_setup_close();
    if (s_ai.root) {
        lv_obj_delete(s_ai.root);
    }
    s_ai.root = NULL;
    s_ai.chat = NULL;
    s_ai.msg_list = NULL;
    s_ai.hint = NULL;
    s_ai.input_ta = NULL;
    s_ai.gear_btn = NULL;
    s_ai.status_label = NULL;

    /* A request still out is left to finish and free itself; one that has
     * finished is kept. */
    assistant_job_t *job = s_ai.job;
    s_ai.job = NULL;
    if (job) {
        int expected = JOB_RUNNING;
        if (!atomic_compare_exchange_strong(&job->state, &expected,
                                            JOB_ABANDONED)) {
            add_message(job->kind, job->text);
            job_free(job);
            assistant_ui_save_conversation();
        }
    }
    free_messages();
}
