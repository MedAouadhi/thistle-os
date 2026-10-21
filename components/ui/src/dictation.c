/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — dictation: hold the microphone key, speak, let go.
 *
 * Two halves. On the LVGL task: the key's edges, the checks at the press,
 * the pill, the toasts, and the text going into the field. On a task of the
 * dictation's own: the recording, into one PSRAM buffer laid out as the
 * request body (the form's head, the WAV header, the samples, the form's
 * tail), then the upload. They share a job; the task writes its result and
 * sets `done` last, and touches nothing after.
 */
#include "ui/dictation.h"
#include "ui/dictation_core.h"
#include "ui/manager.h"
#include "ui/quick_panel.h"
#include "ui/toast.h"

#include "hal/board.h"
#include "hal/sdcard_path.h"
#include "thistle/audio_volume.h"
#include "thistle/call.h"
#include "thistle/dictation_prefs.h"
#include "thistle/net_manager.h"
#include "thistle/power_manager.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#ifdef SIMULATOR_BUILD
#include "esp_system.h"
#else
#include "esp_random.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* COHERE_API_TOKEN from the repository's .env, when the build had one;
 * generated into the build directory by dictation_credentials.cmake. */
#if defined(__has_include)
#if __has_include("dictation_key_default.h")
#include "dictation_key_default.h"
#endif
#endif
#ifndef DICTATION_DEFAULT_API_KEY
#define DICTATION_DEFAULT_API_KEY ""
#endif

static const char *TAG = "dictation";

#define CONFIG_PATH THISTLE_SDCARD "/config/dictation.json"
#define CONFIG_MAX  1024

/* TLS wants the room: the Podcasts threads have as much. */
#define TASK_STACK    8192
#define TASK_PRIORITY 5
/* A read's worth: how long the key's release can wait to be heard. */
#define CHUNK_BYTES   (DICTATION_BYTES_PER_SECOND / 10)
#define ANSWER_MAX    (16 * 1024)
#define TEXT_MAX      4096
#define HTTP_TIMEOUT_MS 30000
#define POLL_MS       50
/* Below the status bar. */
#define PILL_Y        30

typedef enum {
    RESULT_TEXT,
    RESULT_TAP,
    RESULT_NOTHING,
    RESULT_ERROR,
} result_t;

typedef struct {
    dictation_config_t config;
    dictation_form_t form;
    uint8_t *body;        /* head, WAV header, samples, tail */
    size_t pcm_offset;

    atomic_bool stop;      /* LVGL -> task: the key is up */
    atomic_uint captured;  /* task -> LVGL: bytes of samples so far */
    atomic_bool recorded;  /* task -> LVGL: the microphone is done */
    atomic_bool done;      /* task -> LVGL: the result is in */

    result_t result;
    char *text;            /* TEXT_MAX, the transcript */
    char error[96];
    char *answer;          /* ANSWER_MAX, the response body */
    size_t answer_len;
} job_t;

typedef enum {
    PHASE_IDLE,
    PHASE_STARTING,      /* the task is up; no audio yet */
    PHASE_LISTENING,
    PHASE_TRANSCRIBING,
} phase_t;

static struct {
    bool key_down;         /* edges seen, not yet acted on */
    bool key_up;
    phase_t phase;
    job_t *job;
    lv_obj_t *target;      /* the field focused at the press; NULL once deleted */
    lv_obj_t *pill;
    lv_obj_t *pill_label;
    int seconds_left;      /* on the pill, -1 before the countdown */
    char listening[48];    /* the pill's text while recording: "Listening (AR)" */
    uint32_t hold;
} s;

static void *psram_calloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!p) p = malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

/* ------------------------------------------------------------------ */
/* The task: record, then upload                                        */
/* ------------------------------------------------------------------ */

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    job_t *job = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && job && evt->data_len > 0) {
        size_t room = ANSWER_MAX - 1 - job->answer_len;
        size_t n = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
        memcpy(job->answer + job->answer_len, evt->data, n);
        job->answer_len += n;
        job->answer[job->answer_len] = '\0';
    }
    return ESP_OK;
}

/* How loud the recording was, for the log: what to tune the gain by. */
static void log_levels(const uint8_t *pcm, size_t bytes)
{
    size_t samples = bytes / 2;
    if (!samples) return;
    int peak = 0;
    double sum = 0;
    for (size_t i = 0; i < samples; i++) {
        int v = (int16_t)(pcm[2 * i] | (pcm[2 * i + 1] << 8));
        int a = v < 0 ? -v : v;
        if (a > peak) peak = a;
        sum += (double)v * v;
    }
    double rms = sqrt(sum / (double)samples);
    ESP_LOGI(TAG, "%u.%u s recorded; peak %.1f dBFS, rms %.1f dBFS",
             (unsigned)(bytes / DICTATION_BYTES_PER_SECOND),
             (unsigned)(bytes % DICTATION_BYTES_PER_SECOND * 10 / DICTATION_BYTES_PER_SECOND),
             peak ? 20.0 * log10(peak / 32768.0) : -96.0,
             rms > 0 ? 20.0 * log10(rms / 32768.0) : -96.0);
}

/* Which script the transcript came back in, without what it says: the
 * model takes the language it is told, and may answer English speech in
 * Arabic, translated. */
static void log_script(const char *text)
{
    unsigned latin = 0, arabic = 0, other = 0;
    for (const uint8_t *p = (const uint8_t *)text; *p;) {
        uint32_t cp = *p;
        size_t n = 1;
        if (cp >= 0xF0) { cp &= 0x07; n = 4; }
        else if (cp >= 0xE0) { cp &= 0x0F; n = 3; }
        else if (cp >= 0xC0) { cp &= 0x1F; n = 2; }
        for (size_t i = 1; i < n && p[i]; i++) cp = (cp << 6) | (p[i] & 0x3F);
        p += n;
        if (cp < 0x80) latin += (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
        else if (cp >= 0x600 && cp <= 0x6FF) arabic++;
        else other++;
    }
    ESP_LOGI(TAG, "heard %u Latin letters, %u Arabic, %u other", latin, arabic, other);
}

static void upload(job_t *job, size_t pcm)
{
    dictation_wav_header(job->body + job->form.head_len, DICTATION_SAMPLE_RATE, (uint32_t)pcm);
    memcpy(job->body + job->pcm_offset + pcm, job->form.tail, job->form.tail_len);
    size_t total = job->pcm_offset + pcm + job->form.tail_len;

    esp_http_client_config_t config = {
        .url = job->config.url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http_event,
        .user_data = job,
        .user_agent = "ThistleOS",
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        job->result = RESULT_ERROR;
        snprintf(job->error, sizeof(job->error), "Not enough memory to send the recording");
        return;
    }
    char auth[DICTATION_KEY_MAX + 8];
    snprintf(auth, sizeof(auth), "Bearer %s", job->config.api_key);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", job->form.content_type);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, (const char *)job->body, (int)total);
    memset(auth, 0, sizeof(auth));

    int64_t started = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "%u KB sent; answered %d in %u ms", (unsigned)(total / 1024), status,
             (unsigned)((esp_timer_get_time() - started) / 1000));

    if (status == 200) {
        char *text = job->text;
        if (!dictation_json_string(job->answer, job->answer_len, "text", text, TEXT_MAX)) {
            job->result = RESULT_ERROR;
            snprintf(job->error, sizeof(job->error), "Cohere's answer had no text in it");
            return;
        }
        /* Only space is nothing heard, which joining would find too. */
        bool any = false;
        for (const char *p = text; *p && !any; p++) any = *p != ' ' && *p != '\n' && *p != '\r';
        job->result = any ? RESULT_TEXT : RESULT_NOTHING;
        log_script(text);
        return;
    }
    char message[64];
    dictation_json_string(job->answer, job->answer_len, "message", message, sizeof(message));
    if (err != ESP_OK) ESP_LOGW(TAG, "request failed: %s", esp_err_to_name(err));
    else ESP_LOGW(TAG, "refused: %d %s", status, message);
    job->result = RESULT_ERROR;
    dictation_status_words(status, message, job->error, sizeof(job->error));
}

static void dictation_task(void *arg)
{
    job_t *job = arg;
    const hal_audio_driver_t *audio = hal_get_registry()->audio;

    /* A player pauses while anything records, and plays on at its own
     * format after (audio_volume.h). */
    rs_audio_capture_begin();
    hal_audio_config_t format = {
        .sample_rate = DICTATION_SAMPLE_RATE,
        .bits_per_sample = 16,
        .channels = 1,
    };
    esp_err_t err = audio->configure ? audio->configure(&format) : ESP_OK;

    /* The first read opens the codec, which drops what the microphone hears
     * while it settles: the pill says Listening once audio arrives. */
    uint8_t *pcm = job->body + job->pcm_offset;
    size_t got = 0;
    while (err == ESP_OK && !atomic_load(&job->stop) && got < DICTATION_MAX_PCM_BYTES) {
        size_t n = DICTATION_MAX_PCM_BYTES - got;
        if (n > CHUNK_BYTES) n = CHUNK_BYTES;
        err = audio->record(pcm + got, n);
        if (err == ESP_OK) {
            got += n;
            atomic_store(&job->captured, (unsigned)got);
        }
    }
    atomic_store(&job->recorded, true);
    rs_audio_capture_end();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "recording failed: %s", esp_err_to_name(err));
        job->result = RESULT_ERROR;
        snprintf(job->error, sizeof(job->error), "The microphone failed (%s)", esp_err_to_name(err));
    } else if (got < DICTATION_MIN_MS * DICTATION_BYTES_PER_SECOND / 1000) {
        job->result = RESULT_TAP;
    } else {
        log_levels(pcm, got);
        upload(job, got);
    }

#ifndef SIMULATOR_BUILD
    ESP_LOGI(TAG, "done; %u bytes of stack to spare", (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
    atomic_store(&job->done, true);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* The LVGL side                                                        */
/* ------------------------------------------------------------------ */

static void pill_show(const char *text)
{
    if (!s.pill) {
        s.pill = lv_obj_create(lv_layer_top());
        lv_obj_remove_flag(s.pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(s.pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(s.pill, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s.pill, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(s.pill, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_width(s.pill, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(s.pill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(s.pill, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(s.pill, 4, LV_PART_MAIN);
        lv_obj_align(s.pill, LV_ALIGN_TOP_MID, 0, PILL_Y);
        s.pill_label = lv_label_create(s.pill);
        lv_obj_set_style_text_color(s.pill_label, lv_color_white(), LV_PART_MAIN);
    }
    lv_label_set_text(s.pill_label, text);
}

static void pill_hide(void)
{
    if (s.pill) lv_obj_delete(s.pill);
    s.pill = NULL;
    s.pill_label = NULL;
}

static void target_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) == s.target) s.target = NULL;
}

static void set_target(lv_obj_t *field)
{
    if (s.target) lv_obj_remove_event_cb_with_user_data(s.target, target_deleted, NULL);
    s.target = field;
    if (field) lv_obj_add_event_cb(field, target_deleted, LV_EVENT_DELETE, NULL);
}

static void load_config(dictation_config_t *config)
{
    dictation_config_init(config, DICTATION_DEFAULT_API_KEY);
    FILE *f = fopen(CONFIG_PATH, "rb");
    if (!f) return;
    char *text = malloc(CONFIG_MAX);
    if (text) {
        size_t len = fread(text, 1, CONFIG_MAX, f);
        if (!dictation_config_apply(config, text, len)) {
            ESP_LOGW(TAG, "%s is not a JSON object; using the defaults", CONFIG_PATH);
        }
        memset(text, 0, CONFIG_MAX);
        free(text);
    }
    fclose(f);
}

/* The language: the user's, from the quick panel. */
static void load_language(dictation_config_t *config)
{
    char language[DICTATION_LANGUAGE_MAX];
    if (rs_dictation_get_language(language, sizeof(language))) {
        memcpy(config->language, language, sizeof(language));
    }
}

static void job_free(job_t *job)
{
    if (!job) return;
    if (job->config.api_key[0]) memset(job->config.api_key, 0, sizeof(job->config.api_key));
    free(job->body);
    free(job->text);
    free(job->answer);
    free(job);
}

static void say(const char *message)
{
    ESP_LOGI(TAG, "%s", message);
    toast_show(message, TOAST_INFO, 2500);
}

static void start(void)
{
    if (s.job) {
        say("Still transcribing the last one");
        return;
    }
    lv_obj_t *field = ui_manager_focused_field();
    if (!field) {
        say("Pick a text field to dictate into");
        return;
    }
    if (lv_textarea_get_password_mode(field)) {
        say("No dictating into a password field");
        return;
    }
    const hal_registry_t *reg = hal_get_registry();
    if (!reg || !reg->audio || !reg->audio->record) {
        say("This device has no microphone");
        return;
    }
    if (rs_call_in_progress()) {
        say("Not during a call");
        return;
    }
    if (!net_is_connected()) {
        say("Dictation needs a network");
        return;
    }

    job_t *job = psram_calloc(sizeof(*job));
    if (!job) {
        say("Not enough memory to dictate");
        return;
    }
    load_config(&job->config);
    load_language(&job->config);
    if (!job->config.api_key[0]) {
        job_free(job);
        say("Dictation needs a Cohere API key: config/dictation.json");
        return;
    }
    char boundary[40];
    snprintf(boundary, sizeof(boundary), "thistle%08lx%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random());
    if (!dictation_form_build(&job->form, boundary, job->config.model, job->config.language)) {
        job_free(job);
        say("config/dictation.json has a model that cannot be sent");
        return;
    }
    job->pcm_offset = job->form.head_len + DICTATION_WAV_HEADER_BYTES;
    job->body = psram_calloc(job->pcm_offset + DICTATION_MAX_PCM_BYTES + job->form.tail_len);
    job->text = psram_calloc(TEXT_MAX);
    job->answer = psram_calloc(ANSWER_MAX);
    if (!job->body || !job->text || !job->answer) {
        job_free(job);
        say("Not enough memory to dictate");
        return;
    }
    memcpy(job->body, job->form.head, job->form.head_len);

    s.hold = power_manager_hold("dictating");
    if (xTaskCreate(dictation_task, "dictation", TASK_STACK, job, TASK_PRIORITY, NULL) != pdPASS) {
        power_manager_release(s.hold);
        s.hold = 0;
        job_free(job);
        say("Could not start dictating");
        return;
    }
    s.job = job;
    set_target(field);
    s.phase = PHASE_STARTING;
    s.seconds_left = -1;
    char upper[DICTATION_LANGUAGE_MAX];
    size_t i = 0;
    for (; job->config.language[i] && i + 1 < sizeof(upper); i++) {
        char c = job->config.language[i];
        upper[i] = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
    }
    upper[i] = '\0';
    snprintf(s.listening, sizeof(s.listening), LV_SYMBOL_AUDIO " Listening (%s)", upper);
    ESP_LOGI(TAG, "recording, in %s", job->config.language);
}

static void insert(const char *transcript)
{
    if (!s.target) {
        say("The text field closed before the words arrived");
        return;
    }
    size_t cap = strlen(transcript) + 2;
    char *joined = psram_calloc(cap);
    if (!joined) {
        say("Not enough memory to insert the words");
        return;
    }
    const char *text = lv_textarea_get_text(s.target);
    uint8_t before = dictation_byte_before(text, lv_textarea_get_cursor_pos(s.target));
    size_t n = dictation_join(joined, cap, transcript, before, lv_textarea_get_one_line(s.target));
    if (n) lv_textarea_add_text(s.target, joined);
    ESP_LOGI(TAG, "inserted %u bytes", (unsigned)n);
    free(joined);
}

static void finish(void)
{
    job_t *job = s.job;
    switch (job->result) {
    case RESULT_TEXT:    insert(job->text); break;
    case RESULT_TAP:     say("Hold the mic key while you speak"); break;
    case RESULT_NOTHING: say("Heard nothing"); break;
    case RESULT_ERROR:   say(job->error); break;
    }
    pill_hide();
    set_target(NULL);
    job_free(job);
    s.job = NULL;
    s.phase = PHASE_IDLE;
    if (s.hold) power_manager_release(s.hold);
    s.hold = 0;
}

static void stop_recording(void)
{
    if (s.job && (s.phase == PHASE_STARTING || s.phase == PHASE_LISTENING)) {
        atomic_store(&s.job->stop, true);
    }
}

static void dictation_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s.key_down) {
        s.key_down = false;
        start();
    }
    if (s.key_up) {
        s.key_up = false;
        stop_recording();
    }
    job_t *job = s.job;
    if (!job) return;

    unsigned captured = atomic_load(&job->captured);
    if (s.phase == PHASE_STARTING && captured > 0) {
        s.phase = PHASE_LISTENING;
        pill_show(s.listening);
    }
    if (s.phase == PHASE_LISTENING) {
        uint32_t ms = (uint32_t)((uint64_t)captured * 1000 / DICTATION_BYTES_PER_SECOND);
        if (ms >= DICTATION_WARN_MS && ms < DICTATION_MAX_MS) {
            int left = (int)((DICTATION_MAX_MS - ms + 999) / 1000);
            if (left != s.seconds_left) {
                char text[sizeof(s.listening) + 16];
                snprintf(text, sizeof(text), "%s, %d s left", s.listening, left);
                pill_show(text);
                s.seconds_left = left;
            }
        }
    }
    if (atomic_load(&job->recorded) && s.phase != PHASE_TRANSCRIBING) {
        s.phase = PHASE_TRANSCRIBING;
        if (captured >= DICTATION_MIN_MS * DICTATION_BYTES_PER_SECOND / 1000) {
            pill_show("Transcribing...");
        } else {
            pill_hide();
        }
    }
    if (atomic_load(&job->done)) finish();
}

/* The quick panel's switch: on dictates in English, off in Arabic. Cohere
 * is told which, and detects neither. */
static bool can_dictate(void)
{
    const hal_registry_t *reg = hal_get_registry();
    return reg && reg->audio && reg->audio->record;
}

static bool english_is_on(void)
{
    char language[DICTATION_LANGUAGE_MAX];
    return rs_dictation_get_language(language, sizeof(language)) && strcmp(language, "en") == 0;
}

static const char *english_set_on(bool on)
{
    return rs_dictation_set_language(on ? "en" : DICTATION_DEFAULT_LANGUAGE) == ESP_OK
               ? NULL
               : "Not saved for next boot";
}

static const quick_toggle_t s_english = {
    .symbol    = "EN",
    .label     = "Dictation",
    .available = can_dictate,
    .is_on     = english_is_on,
    .set_on    = english_set_on,
};

void dictation_init(void)
{
    lv_timer_create(dictation_timer_cb, POLL_MS, NULL);
    quick_panel_add_toggle(&s_english);
}

void dictation_key(bool down)
{
    if (down) s.key_down = true;
    else s.key_up = true;
}

bool dictation_active(void)
{
    return s.phase != PHASE_IDLE;
}
