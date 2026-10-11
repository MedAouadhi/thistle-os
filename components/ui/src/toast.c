#include "ui/toast.h"
#include "ui/manager.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "toast";

#define TOAST_MAX_W   280
#define TOAST_MARGIN    8   /* screen edge to toast, each side */

static lv_obj_t *s_toast_obj = NULL;
static lv_timer_t *s_dismiss_timer = NULL;

/* Timer callback — auto-dismiss */
static void toast_dismiss_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    toast_dismiss();
}

void toast_dismiss(void)
{
    if (s_dismiss_timer) {
        lv_timer_delete(s_dismiss_timer);
        s_dismiss_timer = NULL;
    }
    if (s_toast_obj) {
        lv_obj_delete(s_toast_obj);
        s_toast_obj = NULL;
    }
}

esp_err_t toast_show(const char *message, toast_level_t level, uint32_t duration_ms)
{
    if (!message) return ESP_ERR_INVALID_ARG;

    /* Dismiss any existing toast first */
    toast_dismiss();

    lv_obj_t *screen = ui_manager_get_screen();
    if (!screen) return ESP_ERR_INVALID_STATE;

    /* Create toast container — positioned at bottom of screen, centered, and
     * no wider than the screen less a margin each side. A fixed 280 px hung
     * off both edges of a 240 px screen, taking the start of the message
     * with it. */
    int32_t toast_w = lv_obj_get_width(screen) - 2 * TOAST_MARGIN;
    if (toast_w > TOAST_MAX_W) toast_w = TOAST_MAX_W;
    s_toast_obj = lv_obj_create(screen);
    lv_obj_set_size(s_toast_obj, toast_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(s_toast_obj, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_toast_obj, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_toast_obj, 6, LV_PART_MAIN);

    /* Position: bottom center, above the dock area */
    lv_obj_align(s_toast_obj, LV_ALIGN_BOTTOM_MID, 0, -70);

    /* Style based on level */
    if (level == TOAST_WARNING || level == TOAST_ERROR) {
        /* Inverted: black bg, white text */
        lv_obj_set_style_bg_color(s_toast_obj, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_toast_obj, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_toast_obj, lv_color_white(), LV_PART_MAIN);
    } else {
        /* Normal: white bg, black text, black border */
        lv_obj_set_style_bg_color(s_toast_obj, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_toast_obj, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_toast_obj, lv_color_black(), LV_PART_MAIN);
    }

    lv_obj_set_style_border_width(s_toast_obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_toast_obj, 4, LV_PART_MAIN);

    /* Bring to front */
    lv_obj_move_foreground(s_toast_obj);

    /* Build message with prefix based on level */
    char display_msg[128];
    const char *prefix = "";
    switch (level) {
        case TOAST_SUCCESS: prefix = "OK "; break;
        case TOAST_WARNING: prefix = "! ";  break;
        case TOAST_ERROR:   prefix = "X ";  break;
        default: break;
    }
    snprintf(display_msg, sizeof(display_msg), "%s%s", prefix, message);

    /* Label */
    lv_obj_t *label = lv_label_create(s_toast_obj);
    lv_label_set_text(label, display_msg);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);

    if (level == TOAST_WARNING || level == TOAST_ERROR) {
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    }

    /* Auto-dismiss timer */
    s_dismiss_timer = lv_timer_create(toast_dismiss_timer_cb, duration_ms, NULL);
    lv_timer_set_repeat_count(s_dismiss_timer, 1);  /* fire once */

    ESP_LOGI(TAG, "toast: %s", message);
    return ESP_OK;
}

esp_err_t toast_info(const char *message)
{
    return toast_show(message, TOAST_INFO, 3000);
}

esp_err_t toast_warn(const char *message)
{
    return toast_show(message, TOAST_WARNING, 4000);
}

/* Toasts posted from outside LVGL, shown by an LVGL timer. Event handlers run
 * on whichever task published the event, and WiFi's come from ESP-IDF's event
 * task. A toast built there raced lvgl_task through LVGL's heap and timer
 * list, and a failed WiFi connect crashed the device a little later. */
#define TOAST_POST_MSG_LEN      64
#define TOAST_POST_QUEUE_LEN     4
#define TOAST_POST_PERIOD_MS    50

typedef struct {
    char          message[TOAST_POST_MSG_LEN];
    toast_level_t level;
    uint32_t      duration_ms;
} posted_toast_t;

static QueueHandle_t s_posted = NULL;

static void toast_post_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    posted_toast_t t;
    /* Several at once would only replace each other; show the latest. */
    bool any = false;
    while (xQueueReceive(s_posted, &t, 0) == pdTRUE) {
        any = true;
    }
    if (any) {
        toast_show(t.message, t.level, t.duration_ms);
    }
}

esp_err_t toast_init(void)
{
    if (s_posted) return ESP_OK;
    s_posted = xQueueCreate(TOAST_POST_QUEUE_LEN, sizeof(posted_toast_t));
    if (!s_posted) return ESP_ERR_NO_MEM;
    lv_timer_create(toast_post_timer_cb, TOAST_POST_PERIOD_MS, NULL);
    return ESP_OK;
}

esp_err_t toast_post(const char *message, toast_level_t level, uint32_t duration_ms)
{
    if (!message) return ESP_ERR_INVALID_ARG;
    if (!s_posted) return ESP_ERR_INVALID_STATE;

    posted_toast_t t = { .level = level, .duration_ms = duration_ms };
    strncpy(t.message, message, sizeof(t.message) - 1);
    t.message[sizeof(t.message) - 1] = '\0';
    return xQueueSend(s_posted, &t, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}
