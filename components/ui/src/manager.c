#include "ui/manager.h"
#include "ui/arabic_text.h"
#include "ui/theme.h"
#include "ui/statusbar.h"
#include "ui/epaper_refresh.h"
#include "ui/toast.h"
#include "ui/power_ui.h"
#include "ui/pull_gesture.h"
#include "ui/quick_panel.h"
#include "ui/call_ui.h"
#include "ui/dictation.h"
#include "thistle/power_manager.h"
#include "hal/board.h"
#include "hal/input.h"
#include "thistle/app_manager.h"
#include "thistle/refresh_prefs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "ui_mgr";

/* Partial rendering keeps draw memory proportional to the actual panel width.
 * Twenty RGB565 lines per buffer is enough for LVGL to batch useful work
 * without reserving hundreds of kilobytes for a hypothetical 480x480 panel. */
#define DRAW_BUF_LINES 20
#define DRAW_BUF_BYTES_PER_PIXEL 2
#define STATUSBAR_H     24
#define LVGL_TASK_PERIOD_MS 10

/* E-paper debounce: wait this many microseconds after the last flush
 * before committing the framebuffer to the physical panel. */
#define EPAPER_REFRESH_DEBOUNCE_US  (300 * 1000)   /* 300 ms */

/* Timestamp (microseconds) of the most recent flush callback. */
static int64_t s_last_flush_time = 0;

/* Whether to run deferred e-paper refresh in the LVGL task.
 * Set during ui_manager_init() based on the WM variant. */
static bool s_use_deferred_refresh = false;

/* Runtime display dimensions — set from HAL in ui_manager_init() */
static uint16_t s_display_w = 240;
static uint16_t s_display_h = 320;

static uint8_t *s_draw_buf1 = NULL;
static uint8_t *s_draw_buf2 = NULL;
static size_t   s_draw_buf_size = 0;

static lv_display_t   *s_display  = NULL;
static lv_obj_t       *s_screen   = NULL;
static lv_obj_t       *s_statusbar_cont = NULL;
static lv_obj_t       *s_app_area = NULL;
static SemaphoreHandle_t s_lvgl_mutex = NULL;

/* -------------------------------------------------------------------------
 * Input device state — written by HAL callbacks, read by LVGL read callbacks.
 * Both callbacks run from lvgl_task (poll loop → lv_timer_handler), so no
 * additional locking is required.
 * ------------------------------------------------------------------------- */
static lv_indev_t *s_touch_indev = NULL;
static struct {
    int16_t x, y;
    lv_indev_state_t state;
} s_touch_state = { 0, 0, LV_INDEV_STATE_RELEASED };

/* A press LVGL has not read yet. A short tap can press and release between
 * two pointer reads, and LVGL reads the pointer only every refresh period, so
 * the press is reported once before the release; otherwise LVGL never sees
 * it. The state itself always follows the finger. It once stayed PRESSED
 * after a release, so LVGL saw a press nobody made, blacked out the button
 * under it, and dropped the next tap. */
static bool s_touch_press_unread = false;

typedef struct {
    uint32_t key;
    lv_indev_state_t state;
} kbd_event_t;

static lv_indev_t *s_kbd_indev = NULL;
/* The keypad as the latest key event left it; reported once the queue below
 * is empty. */
static kbd_event_t s_kbd_state = { 0, LV_INDEV_STATE_RELEASED };

/* Key events LVGL has not read yet, oldest first. The keyboard driver drains
 * its whole event FIFO in one poll, so a key, or several, can go down and up
 * before LVGL next reads the keypad. Reporting s_kbd_state alone, LVGL saw
 * only the last of those events and lost the keys. A full queue drops the
 * event; s_kbd_state still follows the keyboard, so no key is left held. */
#define KBD_QUEUE_LEN 32
static kbd_event_t s_kbd_queue[KBD_QUEUE_LEN];
static uint8_t     s_kbd_queue_head = 0;
static uint8_t     s_kbd_queue_len  = 0;

/* A Home or Back press waiting for the LVGL lock. Input drivers are polled
 * before lvgl_task takes the lock, so the HAL callback only records the press,
 * and an LVGL timer acts on it from inside lv_timer_handler(), lock held, the
 * way a tap on a launcher icon launches an app. */
#define NAV_KEY_TIMER_PERIOD_MS 20
static bool s_home_pending = false;
static bool s_back_pending = false;

/* The quick panel, asked for by a drag (pull_gesture.h) or the quick key,
 * and opened or closed by the same timer. */
static pull_gesture_t s_pull;
static bool s_quick_open_pending = false;
static bool s_quick_close_pending = false;
static bool s_quick_toggle_pending = false;
/* Esc while the call screen is open: offered to it alone, as Back is. */
static bool s_call_esc_pending = false;
static void (*s_home_handler)(void) = NULL;
/* The app in front's back handler, and the root container it belongs to. The
 * app manager destroys an app whenever another comes forward, so deleting the
 * container is what retires the handler; no app has to remember to. */
static lv_obj_t *s_back_owner = NULL;
static bool (*s_back_handler)(void) = NULL;

void ui_manager_set_home_handler(void (*handler)(void))
{
    s_home_handler = handler;
}

static void back_owner_deleted_cb(lv_event_t *e)
{
    /* An app destroyed after the next one registered leaves that one's. */
    if (lv_event_get_target(e) == s_back_owner) {
        s_back_owner   = NULL;
        s_back_handler = NULL;
    }
}

void ui_manager_set_back_handler(lv_obj_t *owner, bool (*handler)(void))
{
    s_back_owner   = owner;
    s_back_handler = owner ? handler : NULL;
    if (owner) {
        lv_obj_add_event_cb(owner, back_owner_deleted_cb, LV_EVENT_DELETE, NULL);
    }
}

/* Back goes to the call screen first, which silences a ringing call or tucks
 * one away; then it closes the quick panel or the power menu, or takes the
 * app in front up one page; false when there was none of those to do. */
static bool go_back(void)
{
    if (call_ui_back()) return true;
    if (quick_panel_close()) return true;
    if (power_ui_close_menu()) return true;
    return s_back_handler && !lv_obj_has_flag(s_back_owner, LV_OBJ_FLAG_HIDDEN) &&
           s_back_handler();
}

static void nav_key_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_quick_close_pending) {
        s_quick_close_pending = false;
        quick_panel_close();
    }
    /* No quick panel over the call screen. */
    if (s_quick_open_pending) {
        s_quick_open_pending = false;
        if (!call_ui_is_open()) quick_panel_open();
    }
    if (s_quick_toggle_pending) {
        s_quick_toggle_pending = false;
        if (!call_ui_is_open()) quick_panel_toggle();
    }
    if (s_call_esc_pending) {
        s_call_esc_pending = false;
        call_ui_back();
    }
    if (s_back_pending) {
        s_back_pending = false;
        if (!go_back()) {
            s_home_pending = true;
        }
    }
    if (!s_home_pending) return;
    s_home_pending = false;

    /* Home tucks a call away, and does nothing while one rings. */
    if (call_ui_home()) return;
    quick_panel_close();
    app_manager_launch("com.thistle.launcher");
    if (s_home_handler) {
        s_home_handler();
    }
}

static void kbd_queue_push(kbd_event_t ev)
{
    if (s_kbd_queue_len == KBD_QUEUE_LEN) {
        ESP_LOGW(TAG, "key queue full, dropping a key event");
        return;
    }
    s_kbd_queue[(s_kbd_queue_head + s_kbd_queue_len) % KBD_QUEUE_LEN] = ev;
    s_kbd_queue_len++;
}

/* Watch the touch stream for the drags that open and close the quick panel.
 * The touch still reaches LVGL: it went down on the status bar, which does
 * nothing with a drag, or on the open panel. */
static void note_pull(pull_touch_t kind, int16_t x, int16_t y)
{
    switch (pull_gesture_feed(&s_pull, kind, x, y, quick_panel_is_open())) {
        case PULL_OPEN:
            if (!call_ui_is_open()) s_quick_open_pending = true;
            break;
        case PULL_CLOSE: s_quick_close_pending = true; break;
        default: break;
    }
}

/* HAL input callback — handles all event types from any registered driver.
 * Touch events update s_touch_state; key events update s_kbd_state.
 * A single callback is registered on every driver so that combined drivers
 * (e.g. the SDL2 simulator driver) work correctly regardless of is_touch. */
static void ui_input_hal_cb(const hal_input_event_t *event, void *user_data)
{
    (void)user_data;
    power_manager_note_activity();
    switch (event->type) {
        case HAL_INPUT_EVENT_TOUCH_DOWN:
        case HAL_INPUT_EVENT_TOUCH_MOVE:
            s_touch_state.x     = (int16_t)event->touch.x;
            s_touch_state.y     = (int16_t)event->touch.y;
            s_touch_state.state = LV_INDEV_STATE_PRESSED;
            s_touch_press_unread = true;
            note_pull(event->type == HAL_INPUT_EVENT_TOUCH_DOWN ? PULL_TOUCH_DOWN
                                                                : PULL_TOUCH_MOVE,
                      s_touch_state.x, s_touch_state.y);
            break;
        case HAL_INPUT_EVENT_TOUCH_UP:
            s_touch_state.state = LV_INDEV_STATE_RELEASED;
            note_pull(PULL_TOUCH_UP, s_touch_state.x, s_touch_state.y);
            break;
        case HAL_INPUT_EVENT_KEY_DOWN:
        case HAL_INPUT_EVENT_KEY_UP: {
            /* The microphone key types nothing either; dictation wants both
             * of its edges. */
            if (event->key.keycode == HAL_KEY_MIC) {
                dictation_key(event->type == HAL_INPUT_EVENT_KEY_DOWN);
                break;
            }
            /* Home, Back and the quick key type nothing, so they never reach
             * LVGL's keypad. */
            if (event->key.keycode == HAL_KEY_HOME || event->key.keycode == HAL_KEY_BACK ||
                event->key.keycode == HAL_KEY_QUICK) {
                if (event->type == HAL_INPUT_EVENT_KEY_DOWN) {
                    if (event->key.keycode == HAL_KEY_HOME) {
                        s_home_pending = true;
                    } else if (event->key.keycode == HAL_KEY_BACK) {
                        s_back_pending = true;
                    } else {
                        s_quick_toggle_pending = true;
                    }
                }
                break;
            }

            uint32_t lv_key = event->key.keycode;
            if      (lv_key == '\n')  lv_key = LV_KEY_ENTER;
            else if (lv_key == '\b')  lv_key = LV_KEY_BACKSPACE;
            else if (lv_key == 0x1B) lv_key = LV_KEY_ESC;
            else if (lv_key == '\t')  lv_key = LV_KEY_NEXT;

            if (event->type == HAL_INPUT_EVENT_KEY_UP) {
                /* A key let go after a later one went down was released
                 * below when that one was pressed. */
                if (lv_key != s_kbd_state.key) break;
                s_kbd_state.state = LV_INDEV_STATE_RELEASED;
                kbd_queue_push(s_kbd_state);
                break;
            }

            /* Esc goes to the call screen, as Back does, and nowhere else:
             * not to the app under it, nor back to the launcher. */
            if (lv_key == LV_KEY_ESC && call_ui_is_open()) {
                s_call_esc_pending = true;
                break;
            }

            /* Esc closes the quick panel, and does nothing else. */
            if (lv_key == LV_KEY_ESC && quick_panel_is_open()) {
                s_quick_close_pending = true;
                break;
            }

            /* Global ESC handler: if foreground app is not the launcher,
             * switch back to the launcher regardless of focus. */
            if (lv_key == LV_KEY_ESC) {
                app_manager_launch("com.thistle.launcher");
            }

            /* LVGL tracks one key: a second press while one is held reads as
             * the first still held, and the second key is never sent. Typing
             * fast overlaps keys like that, so release the held one first. */
            if (s_kbd_state.state == LV_INDEV_STATE_PRESSED) {
                s_kbd_state.state = LV_INDEV_STATE_RELEASED;
                kbd_queue_push(s_kbd_state);
            }
            s_kbd_state.key   = lv_key;
            s_kbd_state.state = LV_INDEV_STATE_PRESSED;
            kbd_queue_push(s_kbd_state);
            break;
        }
        default:
            break;
    }
}

static void ui_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_touch_state.x;
    data->point.y = s_touch_state.y;

    if (s_touch_press_unread) {
        data->state = LV_INDEV_STATE_PRESSED;
        s_touch_press_unread = false;
    } else {
        data->state = s_touch_state.state;
    }
}

static void ui_kbd_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    kbd_event_t ev = s_kbd_state;
    if (s_kbd_queue_len > 0) {
        ev = s_kbd_queue[s_kbd_queue_head];
        s_kbd_queue_head = (uint8_t)((s_kbd_queue_head + 1) % KBD_QUEUE_LEN);
        s_kbd_queue_len--;
        /* LVGL calls back at once while this is set, so the whole queue is
         * handled in this read, in order. */
        data->continue_reading = s_kbd_queue_len > 0;
    }
    data->key   = ev.key;
    data->state = ev.state;
}

/* -------------------------------------------------------------------------
 * LVGL flush callbacks — separate variants for e-paper and LCD
 * ------------------------------------------------------------------------- */

void ui_flush_cb_epaper(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const hal_registry_t *reg = hal_get_registry();

    if (reg && reg->display && reg->display->flush) {
        hal_area_t hal_area = {
            .x1 = (uint16_t)area->x1,
            .y1 = (uint16_t)area->y1,
            .x2 = (uint16_t)area->x2,
            .y2 = (uint16_t)area->y2,
        };

        /* E-paper displays need 1-bit data, LVGL outputs RGB565. Convert.
         * Each row starts on a fresh byte, (w + 7) / 8 bytes per row, which is
         * how the e-paper drivers read it. Rows packed back to back sheared
         * every area narrower than a multiple of 8 pixels, such as a label
         * that a timer rewrites on its own. */
        uint16_t w = hal_area.x2 - hal_area.x1 + 1;
        uint16_t h = hal_area.y2 - hal_area.y1 + 1;
        uint32_t row_bytes = ((uint32_t)w + 7) / 8;
        uint32_t mono_bytes = row_bytes * h;
        uint8_t *mono_buf = (uint8_t *)malloc(mono_bytes);
        esp_err_t err;
        if (mono_buf) {
            memset(mono_buf, 0, mono_bytes);
            const uint16_t *rgb565 = (const uint16_t *)px_map;
            for (uint16_t y = 0; y < h; y++) {
                uint8_t *row = mono_buf + (uint32_t)y * row_bytes;
                for (uint16_t x = 0; x < w; x++) {
                    uint16_t c = *rgb565++;
                    /* Luminance threshold: white if bright enough */
                    uint8_t r5 = (c >> 11) & 0x1F;
                    uint8_t g6 = (c >> 5) & 0x3F;
                    uint8_t b5 = c & 0x1F;
                    /* Weighted luminance (BT.601): 0.299R + 0.587G + 0.114B.
                     * Green has six bits, so white is 31*77 + 63*150 + 31*29
                     * = 12736, and half of that is the cut. Halving 8160, as
                     * if green had five, cut at a third: a pixel needed two
                     * thirds ink to show, and the dots of Arabic letters, which
                     * antialiasing spreads over a few lighter pixels, went. */
                    uint16_t lum = r5 * 77 + g6 * 150 + b5 * 29;
                    if (lum > 6368) {
                        row[x / 8] |= (0x80 >> (x & 7));  /* white */
                    }
                }
            }
            err = reg->display->flush(&hal_area, mono_buf);
            free(mono_buf);
        } else {
            err = ESP_ERR_NO_MEM;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HAL flush failed: %s", esp_err_to_name(err));
        }

        /* Track dirty region for e-paper batching */
        epaper_refresh_mark_dirty(hal_area.x1, hal_area.y1, hal_area.x2, hal_area.y2);

        /* Record timestamp so the render loop can debounce the panel refresh */
        s_last_flush_time = esp_timer_get_time();
    }

    lv_display_flush_ready(disp);
}

void ui_flush_cb_lcd(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const hal_registry_t *reg = hal_get_registry();

    if (reg && reg->display && reg->display->flush) {
        hal_area_t hal_area = {
            .x1 = (uint16_t)area->x1,
            .y1 = (uint16_t)area->y1,
            .x2 = (uint16_t)area->x2,
            .y2 = (uint16_t)area->y2,
        };

        esp_err_t err = reg->display->flush(&hal_area, px_map);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HAL flush failed: %s", esp_err_to_name(err));
        }
    }

    lv_display_flush_ready(disp);
}

/* -------------------------------------------------------------------------
 * LVGL tick provider — called by esp_timer every 1 ms
 * ------------------------------------------------------------------------- */
static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

/* Whether this task may stop for sleep: the sleep screen is up, and the
 * panel is not still waiting to show it. Stopped earlier, the device would
 * sleep showing whatever was there before. */
static bool ready_to_sleep(void)
{
    if (!power_ui_sleep_screen_shown()) {
        return false;
    }
    return !(s_use_deferred_refresh && (epaper_refresh_is_dirty() || s_last_flush_time > 0));
}

/* -------------------------------------------------------------------------
 * LVGL timer handler task — runs lv_timer_handler() every 10 ms
 * ------------------------------------------------------------------------- */
static void lvgl_task(void *arg)
{
    (void)arg;
    const hal_registry_t *reg = hal_get_registry();
    /* It reads the input drivers and draws: before the device sleeps, it
     * stops at the checkpoint, so sleep never lands mid-transfer. */
    power_ui_join();
    while (1) {
        power_ui_checkpoint(ready_to_sleep());

        /* Poll all registered input drivers so they fire HAL callbacks */
        if (reg) {
            for (int i = 0; i < reg->input_count; i++) {
                if (reg->inputs[i] && reg->inputs[i]->poll) {
                    reg->inputs[i]->poll();
                }
            }
        }

        if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
        }

        /* ── Debounced e-paper panel refresh ──
         * After LVGL flushes stop arriving (UI settled for 300 ms), commit
         * the in-memory framebuffer to the physical e-paper panel once.
         * Only runs when the WM requested deferred refresh mode. */
        if (s_use_deferred_refresh &&
            reg && reg->display &&
            reg->display->refresh &&
            epaper_refresh_is_dirty() &&
            s_last_flush_time > 0)
        {
            int64_t now = esp_timer_get_time();
            if ((now - s_last_flush_time) >= EPAPER_REFRESH_DEBOUNCE_US) {
                /* The driver is told on every refresh, so the policy is the
                 * only thing that decides: drivers drop back to fast after a
                 * full refresh of their own accord. */
                bool full = epaper_refresh_next_is_full();
                if (reg->display->set_refresh_mode) {
                    reg->display->set_refresh_mode(full ? HAL_DISPLAY_REFRESH_FULL
                                                        : HAL_DISPLAY_REFRESH_FAST);
                }
                ESP_LOGI(TAG, "e-paper debounce elapsed, refreshing panel (%s)",
                         full ? "full" : "fast");
                esp_err_t ref_err = reg->display->refresh();
                if (ref_err != ESP_OK) {
                    ESP_LOGE(TAG, "e-paper refresh failed: %s",
                             esp_err_to_name(ref_err));
                }
                epaper_refresh_clear();
                s_last_flush_time = 0;  /* reset until next flush */
            }
        }

        vTaskDelay(pdMS_TO_TICKS(LVGL_TASK_PERIOD_MS));
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t ui_manager_init(ui_flush_fn_t flush_cb, bool use_deferred_refresh)
{
    ESP_LOGI(TAG, "initializing UI manager (deferred_refresh=%d)", use_deferred_refresh);
    s_use_deferred_refresh = use_deferred_refresh;

    /* Read actual display dimensions from HAL */
    const hal_registry_t *reg = hal_get_registry();
    if (reg && reg->display) {
        s_display_w = reg->display->width;
        s_display_h = reg->display->height;
    }
    ESP_LOGI(TAG, "display: %dx%d", s_display_w, s_display_h);

    const size_t draw_lines = s_display_h < DRAW_BUF_LINES
                                  ? s_display_h : DRAW_BUF_LINES;
    s_draw_buf_size = (size_t)s_display_w * draw_lines *
                      DRAW_BUF_BYTES_PER_PIXEL;
#ifdef CONFIG_SPIRAM
    s_draw_buf1 = heap_caps_malloc(s_draw_buf_size,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_draw_buf2 = heap_caps_malloc(s_draw_buf_size,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (s_draw_buf1 == NULL) {
        s_draw_buf1 = heap_caps_malloc(s_draw_buf_size, MALLOC_CAP_8BIT);
    }
    if (s_draw_buf2 == NULL) {
        s_draw_buf2 = heap_caps_malloc(s_draw_buf_size, MALLOC_CAP_8BIT);
    }
    if (s_draw_buf1 == NULL || s_draw_buf2 == NULL) {
        ESP_LOGE(TAG, "draw buffer allocation failed (%zu bytes each)",
                 s_draw_buf_size);
        free(s_draw_buf1);
        free(s_draw_buf2);
        s_draw_buf1 = NULL;
        s_draw_buf2 = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 1. Initialize LVGL */
    lv_init();

    /* 2. Create LVGL display with actual dimensions */
    s_display = lv_display_create(s_display_w, s_display_h);
    if (s_display == NULL) {
        ESP_LOGE(TAG, "lv_display_create failed");
        free(s_draw_buf1);
        free(s_draw_buf2);
        s_draw_buf1 = NULL;
        s_draw_buf2 = NULL;
        return ESP_FAIL;
    }

    /* 3. Set draw buffers (double-buffered, partial rendering) */
    lv_display_set_buffers(s_display,
                           s_draw_buf1, s_draw_buf2,
                           s_draw_buf_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* 4. Set flush callback (provided by the WM variant) */
    lv_display_set_flush_cb(s_display, flush_cb);

    /* 4a. Register LVGL pointer input device (touch / mouse) */
    s_touch_indev = lv_indev_create();
    lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_touch_indev, ui_touch_read_cb);

    /* 4b. Register LVGL keypad input device (hardware keyboard) */
    s_kbd_indev = lv_indev_create();
    lv_indev_set_type(s_kbd_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_kbd_indev, ui_kbd_read_cb);

    /* 4b1. Act on a Home or Back press from inside lv_timer_handler() */
    lv_timer_create(nav_key_timer_cb, NAV_KEY_TIMER_PERIOD_MS, NULL);

    /* 4b1a. Show toasts other tasks post, the same way */
    if (toast_init() != ESP_OK) {
        ESP_LOGW(TAG, "toast_init failed; posted toasts will not show");
    }

    /* 4b1b. The sleep screen and the power menu, when the power manager asks */
    power_ui_init();

    /* 4b1c. The quick panel, opened by a drag down from the status bar */
    quick_panel_add_builtin();
    pull_gesture_init(&s_pull, STATUSBAR_H, (int16_t)s_display_h);

    /* 4b1d. The call screen, over everything, while a call is up */
    call_ui_init();

    /* 4b1e. Dictation, held on the microphone key */
    dictation_init();

    /* Touch stays in TIMER mode — LVGL polls at each lv_timer_handler call */

    /* 4b2. Create a default input group so keyboard events reach focused widgets */
    lv_group_t *default_group = lv_group_create();
    lv_group_set_default(default_group);
    lv_indev_set_group(s_kbd_indev, default_group);

    /* 4c. Wire HAL input callbacks to every registered input driver.
     *     A single combined callback handles both touch and key events so
     *     that drivers which emit both types (e.g. SDL2 simulator) work
     *     correctly regardless of their is_touch flag. */
    {
        const hal_registry_t *reg = hal_get_registry();
        if (reg) {
            for (int i = 0; i < reg->input_count; i++) {
                if (reg->inputs[i] && reg->inputs[i]->register_callback) {
                    reg->inputs[i]->register_callback(ui_input_hal_cb, NULL);
                }
            }
        }
    }

    /* 5. Create mutex */
    s_lvgl_mutex = xSemaphoreCreateMutex();
    if (s_lvgl_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create LVGL mutex");
        return ESP_ERR_NO_MEM;
    }

    /* 6a. Start 1 ms tick timer */
    const esp_timer_create_args_t tick_timer_args = {
        .callback        = lvgl_tick_cb,
        .name            = "lvgl_tick",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_timer_handle_t tick_timer;
    esp_err_t err = esp_timer_create(&tick_timer_args, &tick_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create tick timer: %s", esp_err_to_name(err));
        return err;
    }
    esp_timer_start_periodic(tick_timer, 1000); /* 1 ms */

    /* 7. Build screen layout (LVGL task starts AFTER layout is complete) */
    s_screen = lv_display_get_screen_active(s_display);
    lv_obj_set_size(s_screen, s_display_w, s_display_h);

    /* 7a. Arabic in every label and text field, in fonts for this display */
    arabic_text_init(s_display);

    /* Status bar container — top 24 px */
    s_statusbar_cont = lv_obj_create(s_screen);
    lv_obj_set_pos(s_statusbar_cont, 0, 0);
    lv_obj_set_size(s_statusbar_cont, s_display_w, STATUSBAR_H);
    lv_obj_set_style_pad_all(s_statusbar_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_statusbar_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_statusbar_cont, 0, LV_PART_MAIN);

    /* App content area — below status bar */
    s_app_area = lv_obj_create(s_screen);
    lv_obj_set_pos(s_app_area, 0, STATUSBAR_H);
    lv_obj_set_size(s_app_area, s_display_w, s_display_h - STATUSBAR_H);
    lv_obj_set_style_pad_all(s_app_area, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_app_area, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_app_area, 0, LV_PART_MAIN);

    /* 8. Apply default theme FIRST (statusbar reads theme colors) */
    err = theme_init(s_display);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "theme_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 8a. Apply theme bg color to screen and app area */
    {
        const theme_colors_t *colors = theme_get_colors();
        lv_obj_set_style_bg_color(s_screen,   colors->bg, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_screen,     LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_app_area, colors->bg, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_app_area,   LV_OPA_COVER, LV_PART_MAIN);
    }

    /* 9. Create status bar widgets (uses theme colors) */
    err = statusbar_create(s_statusbar_cont);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "statusbar_create failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 10. Initialize e-paper refresh tracker */
    err = epaper_refresh_init(s_display_w, s_display_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "epaper_refresh_init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (use_deferred_refresh) {
        epaper_refresh_set_policy((epaper_refresh_policy_t)refresh_prefs_load());
        ESP_LOGI(TAG, "e-paper refresh policy: %d", (int)epaper_refresh_get_policy());
    }

    ESP_LOGI(TAG, "UI manager ready (%dx%d)", s_display_w, s_display_h);

    /* NOTE: Splash screen is now the responsibility of the LCD WM variant.
     * LVGL render task is NOT started here — call ui_manager_start()
     * after all LVGL objects (apps, launcher) are fully created. */
    return ESP_OK;
}

esp_err_t ui_manager_start(void)
{
    BaseType_t xret = xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, NULL);
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "failed to create LVGL task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "LVGL render task started");
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Splash screen — full-screen overlay that auto-dismisses via esp_timer
 * ------------------------------------------------------------------------- */

static esp_timer_handle_t s_splash_timer = NULL;

static void splash_dismiss_cb(void *arg)
{
    lv_obj_t *splash = (lv_obj_t *)arg;

    ui_manager_lock();
    lv_obj_delete(splash);
    ui_manager_unlock();

    /* Make the e-paper's next refresh full, whatever the policy, to clear
     * splash ghosting */
    epaper_refresh_request_full();

    /* Clean up the timer handle so it does not leak. */
    if (s_splash_timer != NULL) {
        esp_timer_delete(s_splash_timer);
        s_splash_timer = NULL;
    }
}

void ui_manager_show_splash(uint32_t duration_ms)
{
    if (s_screen == NULL) {
        ESP_LOGW(TAG, "ui_manager_show_splash called before screen ready");
        return;
    }

    /* Full-screen overlay on top of everything */
    lv_obj_t *splash = lv_obj_create(s_screen);
    lv_obj_set_pos(splash, 0, 0);
    lv_obj_set_size(splash, s_display_w, s_display_h);
    lv_obj_set_style_bg_color(splash, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(splash, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(splash, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(splash, 0, LV_PART_MAIN);
    lv_obj_clear_flag(splash, LV_OBJ_FLAG_SCROLLABLE);

    /* Center column layout */
    lv_obj_set_flex_flow(splash, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(splash,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(splash, 8, LV_PART_MAIN);

    /* "ThistleOS" title in large font */
    lv_obj_t *lbl_title = lv_label_create(splash);
    lv_label_set_text(lbl_title, "ThistleOS");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_title, lv_color_black(), LV_PART_MAIN);

    /* Version subtitle */
    lv_obj_t *lbl_ver = lv_label_create(splash);
    lv_label_set_text(lbl_ver, "v0.1.0");
    lv_obj_set_style_text_font(lbl_ver, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_ver, lv_color_black(), LV_PART_MAIN);

    /* Bring to front so it covers the app area and status bar */
    lv_obj_move_foreground(splash);

    /* One-shot timer to delete the splash after duration_ms */
    const esp_timer_create_args_t splash_timer_args = {
        .callback        = splash_dismiss_cb,
        .arg             = (void *)splash,
        .name            = "splash_dismiss",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_err_t err = esp_timer_create(&splash_timer_args, &s_splash_timer);
    if (err == ESP_OK) {
        esp_timer_start_once(s_splash_timer, (uint64_t)duration_ms * 1000ULL);
    } else {
        ESP_LOGE(TAG, "failed to create splash timer: %s", esp_err_to_name(err));
        s_splash_timer = NULL;
    }

    ESP_LOGI(TAG, "splash screen shown for %" PRIu32 " ms", duration_ms);
}

lv_obj_t *ui_manager_get_app_area(void)
{
    return s_app_area;
}

lv_obj_t *ui_manager_get_screen(void)
{
    return s_screen;
}

void ui_manager_request_refresh(void)
{
    /* Mark full screen dirty — LVGL's next timer tick will trigger flush */
    epaper_refresh_mark_full();
    if (s_display != NULL) {
        lv_obj_invalidate(s_screen);
    }
}

void ui_manager_force_refresh(void)
{
    epaper_refresh_mark_full();
    epaper_refresh_request_full();

    if (s_display != NULL) {
        lv_obj_invalidate(s_screen);
    }
}

void ui_manager_lock(void)
{
    if (s_lvgl_mutex != NULL) {
        xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    }
}

void ui_manager_unlock(void)
{
    if (s_lvgl_mutex != NULL) {
        xSemaphoreGive(s_lvgl_mutex);
    }
}
