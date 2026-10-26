#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"

#include "thistle/kernel.h"
#include "thistle/app_manager.h"
#include "thistle/permissions.h"
#include "thistle/event.h"
#include "thistle/ota.h"
#include "thistle/display_server.h"
#include "thistle/elf_loader.h"
#include "thistle/board_config.h"
#include "thistle/power_manager.h"
#include "hal/board.h"
#include "hal/input.h"
#ifdef THISTLE_LEGACY_C_APPS
#include "ui/manager.h"
#include "ui/lvgl_wm.h"
#include "ui/toast.h"
#include "lvgl_apps.h"
#endif

extern const display_server_wm_t *thistle_tk_wm_get(void);

#ifdef CONFIG_THISTLE_RUN_TESTS
#include "unity.h"
static void run_tests(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
#endif

static const char *TAG = "thistle";

#define TK_RENDER_STACK_SIZE 16384

#if CONFIG_IDF_TARGET_ESP32S3
static StackType_t *s_tk_render_stack;
static StaticTask_t s_tk_render_tcb;

static esp_err_t tk_render_stack_reserve(void)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    s_tk_render_stack = heap_caps_malloc(
        TK_RENDER_STACK_SIZE,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_tk_render_stack) {
        ESP_LOGE(TAG,
                 "tk_render: unable to reserve %u-byte internal stack "
                 "(free=%u largest=%u)",
                 TK_RENDER_STACK_SIZE,
                 (unsigned)free_internal,
                 (unsigned)largest_internal);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "tk_render: reserved %u-byte internal stack "
             "(free_before=%u largest_before=%u)",
             TK_RENDER_STACK_SIZE,
             (unsigned)free_internal,
             (unsigned)largest_internal);
    return ESP_OK;
}
#endif

extern void tk_wm_do_refresh(void);
extern bool tk_wm_on_input(const hal_input_event_t *event);
extern void tk_launcher_tick(void);

/* HAL → WM input path. The thistle-tk side has no equivalent of the LVGL
 * ui_input_hal_cb wiring (components/ui/src/manager.c); without this, key and
 * touch events sit in driver FIFOs forever.
 *
 * Events the hardware has given up but the window manager has not seen yet.
 *
 * Reading the keyboard and drawing the screen used to share one task, and an
 * e-paper refresh blocks it for about 650ms. The keyboard chip's own FIFO is
 * ten events deep and a keystroke costs two of them, so five keystrokes typed
 * during one refresh filled it and everything after was lost in the chip. It
 * was a feedback loop, too: each key that did arrive started the next refresh.
 * Typing "keyboard testing" on a T-Deck Max delivered the "k" and nothing else.
 *
 * So the hardware is drained by its own task, promptly, into this queue, and
 * the render task takes events off it when it is ready. Only the render task
 * ever calls into the WM, so the WM keeps its single-threaded contract and
 * needs no locking.
 *
 * Depth covers a burst at typing speed across one refresh with room to spare;
 * the chip's ten-deep FIFO is the part that could not be made bigger. */
#define TK_INPUT_QUEUE_LEN   64
#define TK_INPUT_STACK_SIZE  3072
#define TK_INPUT_POLL_MS     20

static QueueHandle_t s_tk_input_queue;
static uint32_t s_tk_input_dropped;

/* Runs on the input task. Puts the event on the queue and returns — it must
 * not touch the WM, which belongs to the render task. */
static void tk_input_hal_cb(const hal_input_event_t *event, void *user_data)
{
    (void)user_data;
    power_manager_note_activity();
    if (!s_tk_input_queue) return;

    if (xQueueSend(s_tk_input_queue, event, 0) != pdTRUE) {
        /* The render task is further behind than a full refresh. Count it
         * rather than block the drain: blocking here would stall the very
         * poll loop that keeps the chip's FIFO from overflowing, which is
         * the failure this queue exists to prevent. */
        s_tk_input_dropped++;
    }
}

/* Runs on the render task. Hands the WM what the input task collected, and
 * traces presses — not releases or drags, so a held finger does not produce a
 * line every poll. */
static void tk_drain_input_queue(void)
{
    hal_input_event_t event;

    if (!s_tk_input_queue) return;

    while (xQueueReceive(s_tk_input_queue, &event, 0) == pdTRUE) {
        if (event.type == HAL_INPUT_EVENT_KEY_DOWN) {
            ESP_LOGI(TAG, "input: key 0x%02x", event.key.keycode);
        } else if (event.type == HAL_INPUT_EVENT_TOUCH_DOWN) {
            ESP_LOGI(TAG, "input: touch %u,%u", event.touch.x, event.touch.y);
        }
        tk_wm_on_input(&event);
    }

    if (s_tk_input_dropped) {
        ESP_LOGW(TAG, "input: dropped %u event(s); the queue filled",
                 (unsigned)s_tk_input_dropped);
        s_tk_input_dropped = 0;
    }
}

static void tk_register_input_callbacks(void)
{
    const hal_registry_t *reg = hal_get_registry();
    if (!reg) return;
    for (int i = 0; i < reg->input_count; i++) {
        if (reg->inputs[i] && reg->inputs[i]->register_callback) {
            reg->inputs[i]->register_callback(tk_input_hal_cb, NULL);
        }
    }
}

static void tk_poll_inputs(void)
{
    const hal_registry_t *reg = hal_get_registry();
    if (!reg) return;
    for (int i = 0; i < reg->input_count; i++) {
        if (reg->inputs[i] && reg->inputs[i]->poll) {
            reg->inputs[i]->poll();
        }
    }
}

/* Reads the hardware and nothing else, so that how long the screen takes to
 * draw cannot decide whether a keystroke is seen. */
static void tk_input_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "tk_input_task: started");
    power_ui_join();
    for (;;) {
        power_ui_checkpoint(true);
        tk_poll_inputs();
        vTaskDelay(pdMS_TO_TICKS(TK_INPUT_POLL_MS));
    }
}

static void tk_render_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "tk_render_task: started");
    /* Wait a bit to let the system settle before first render */
    vTaskDelay(pdMS_TO_TICKS(500));
    /* Render loop: hand the WM whatever the input task has collected, so any
     * keypresses mark widgets dirty before we render. tk_wm_render() and
     * tk_wm_do_refresh() both internally gate on dirty / REFRESH_NEEDED flags,
     * so they're cheap no-ops when nothing has changed. Hardware refresh only
     * fires after a render actually touched the framebuffer — keeping the
     * e-paper static.
     *
     * Reading the hardware is deliberately not done here: a refresh blocks
     * this task for about 650ms, which is long enough to overflow the
     * keyboard's FIFO. See the input queue above.
     *
     * Refresh is invoked at this shallow call depth (not from inside the
     * deep render chain) to avoid Xtensa CALL8 register-window overflow. */
    bool watermark_logged = false;
    power_ui_join();
    for (;;) {
        power_ui_checkpoint(true);
        tk_drain_input_queue();
        tk_launcher_tick();
        display_server_tick();
        tk_wm_do_refresh();
        if (!watermark_logged) {
            ESP_LOGI(TAG, "tk_render: stack high-water mark after first refresh: %u bytes",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            watermark_logged = true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

#ifdef THISTLE_LEGACY_C_APPS
/* Runs on whichever task published the event (WiFi's: ESP-IDF's event task),
 * never inside LVGL, so it posts its toasts rather than showing them. */
static void system_event_toast(const event_t *event, void *user_data)
{
    (void)user_data;
    switch (event->type) {
        case EVENT_WIFI_CONNECTED:
            toast_post("WiFi connected", TOAST_SUCCESS, 3000);
            break;
        case EVENT_WIFI_DISCONNECTED:
            toast_post("WiFi disconnected", TOAST_INFO, 3000);
            break;
        case EVENT_SD_MOUNTED:
            toast_post("SD card mounted", TOAST_INFO, 3000);
            break;
        case EVENT_SD_UNMOUNTED:
            toast_post("SD card removed", TOAST_WARNING, 4000);
            break;
        case EVENT_BATTERY_LOW:
            toast_post("Battery low!", TOAST_WARNING, 4000);
            break;
        default:
            break;
    }
}
#endif

void app_main(void)
{
#ifdef CONFIG_THISTLE_RUN_TESTS
    run_tests();
    return;
#endif

    ESP_LOGI(TAG, "ThistleOS %s starting...", ota_get_current_version());

#if CONFIG_IDF_TARGET_ESP32S3
    /* Reserve the render stack before Wi-Fi and the driver graph fragment
     * internal DRAM. It remains caller-owned and is attached to the task only
     * after the board and WM have been selected. */
    if (tk_render_stack_reserve() != ESP_OK) {
        return;
    }
#endif

    /* Start kernel services: board init, driver manager, app manager, event bus, IPC */
    esp_err_t ret = kernel_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "kernel_init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Initialize the display server before selecting a window manager. */
    ret = display_server_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "display_server_init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Non-S3 targets now use the Rust WM and signed ELF apps exclusively.
     * S3 keeps the LVGL/C app bridge while that final migration is completed. */
    bool use_tk_wm = true;
#ifdef THISTLE_LEGACY_C_APPS
    {
        /* A display that can refresh is e-paper; that is what chose the
         * manager outright before system.json got a say. It still decides
         * when nothing is configured, so a board that says nothing boots
         * exactly as it did. */
        const hal_registry_t *reg = hal_get_registry();
        bool is_epaper = reg && reg->display && reg->display->refresh;
        thistle_wm_choice_t choice =
            thistle_wm_select(board_config_get_wm_name(), is_epaper);

        ESP_LOGI(TAG, "Window manager: %s (%s built-in LVGL apps)",
                 choice.wm->name, choice.hosts_lvgl_apps ? "with" : "without");
        ret = display_server_register_wm(choice.wm);
        use_tk_wm = !choice.hosts_lvgl_apps;
    }
#else
    ret = display_server_register_wm(thistle_tk_wm_get());
#endif
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register window manager: %s", esp_err_to_name(ret));
        return;
    }

#if CONFIG_IDF_TARGET_ESP32S3
    if (!use_tk_wm && s_tk_render_stack) {
        heap_caps_free(s_tk_render_stack);
        s_tk_render_stack = NULL;
    }
#endif

#ifdef THISTLE_LEGACY_C_APPS
    /* Subscribe to system events that warrant user-visible toasts. */
    event_subscribe(EVENT_WIFI_CONNECTED,    system_event_toast, NULL);
    event_subscribe(EVENT_WIFI_DISCONNECTED, system_event_toast, NULL);
    event_subscribe(EVENT_SD_MOUNTED,        system_event_toast, NULL);
    event_subscribe(EVENT_SD_UNMOUNTED,      system_event_toast, NULL);
    event_subscribe(EVENT_BATTERY_LOW,       system_event_toast, NULL);
#endif

    if (use_tk_wm) {
        /* thistle-tk's own App Store and Flashlight. Under an LVGL manager
         * the LVGL ones take these IDs instead. */
        esp_err_t tk_err = tk_native_apps_register();
        if (tk_err != ESP_OK) {
            ESP_LOGW(TAG, "thistle-tk apps failed to register: %s",
                     esp_err_to_name(tk_err));
        }
    }

#ifdef THISTLE_LEGACY_C_APPS
    if (!use_tk_wm) {
        /* LVGL-based built-in apps; they would crash under the thistle-tk WM.
         * The firmware builds every app and offers all of them whatever the
         * board carries, which is what passing both capabilities preserves. */
        esp_err_t app_err = lvgl_apps_register_builtin(true, true);
        if (app_err != ESP_OK) {
            ESP_LOGW(TAG, "Some built-in apps failed to register: %s",
                     esp_err_to_name(app_err));
        }
    }
#endif

    /* Scan SPIFFS and SD card for standalone .app.elf files.
     * This function is #[no_mangle] in Rust (elf_loader.rs). */
    elf_app_scan_and_register();

    /* Grant full permissions to built-in apps */
    permissions_grant("com.thistle.tk_launcher", PERM_ALL);
#ifdef THISTLE_LEGACY_C_APPS
    permissions_grant("com.thistle.launcher",   PERM_ALL);
    permissions_grant("com.thistle.settings",   PERM_ALL);
    permissions_grant("com.thistle.filemgr",    PERM_ALL);
    permissions_grant("com.thistle.reader",     PERM_ALL);
    permissions_grant("com.thistle.podcasts",   PERM_NETWORK | PERM_STORAGE | PERM_AUDIO);
    permissions_grant("com.thistle.messenger",  PERM_RADIO | PERM_IPC);
    permissions_grant("com.thistle.navigator",  PERM_GPS | PERM_STORAGE);
    permissions_grant("com.thistle.appstore",   PERM_STORAGE | PERM_NETWORK);
    permissions_grant("com.thistle.assistant",   PERM_NETWORK | PERM_STORAGE);
    permissions_grant("com.thistle.terminal",    PERM_ALL);
    permissions_grant("com.thistle.vault",       PERM_STORAGE | PERM_SYSTEM);
#endif

    if (use_tk_wm) {
        /* The queue must exist before any driver can call back into it. */
        s_tk_input_queue = xQueueCreate(TK_INPUT_QUEUE_LEN,
                                        sizeof(hal_input_event_t));
        if (!s_tk_input_queue) {
            ESP_LOGE(TAG, "tk_input: queue allocation failed");
            return;
        }

        /* Wire HAL input drivers (keyboard, touch) to the thistle-tk WM.
         * Must happen after the WM is registered but before the render task
         * starts polling. The LVGL path does the equivalent inside ui_init. */
        tk_register_input_callbacks();

        /* Higher priority than the render task: draining the keyboard is
         * quick and must not wait behind a screen refresh, which is the whole
         * reason these are two tasks. */
        if (xTaskCreate(tk_input_task, "tk_input", TK_INPUT_STACK_SIZE,
                        NULL, 6, NULL) != pdPASS) {
            ESP_LOGE(TAG, "tk_input: xTaskCreate failed");
            return;
        }

        /* Launch the thistle-tk native launcher */
        ret = app_manager_launch("com.thistle.tk_launcher");
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to launch thistle-tk launcher: %s",
                     esp_err_to_name(ret));
            return;
        }

        /* Drive the display server for e-paper.
         * E-paper refresh is slow so we check every 100 ms; the WM
         * internally skips physical refresh when nothing has changed. */
        /* Use an internal DRAM stack — Xtensa register window underflow/overflow
         * during vTaskDelay context switches is unreliable with PSRAM-backed stacks
         * because the saved register windows may be read incorrectly on resume. */
#if CONFIG_IDF_TARGET_ESP32S3
        TaskHandle_t render_task = xTaskCreateStatic(
            tk_render_task, "tk_render", TK_RENDER_STACK_SIZE, NULL, 5,
            s_tk_render_stack, &s_tk_render_tcb);
        if (!render_task) {
            ESP_LOGE(TAG, "tk_render: xTaskCreateStatic failed");
            heap_caps_free(s_tk_render_stack);
            s_tk_render_stack = NULL;
            return;
        }
#else
        BaseType_t task_ret = xTaskCreate(
            tk_render_task, "tk_render", TK_RENDER_STACK_SIZE, NULL, 5, NULL);
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "tk_render: xTaskCreate failed (%d)", task_ret);
            return;
        }
#endif
#ifdef THISTLE_LEGACY_C_APPS
    } else {
        ret = app_manager_launch("com.thistle.launcher");
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to launch LVGL launcher: %s",
                     esp_err_to_name(ret));
            return;
        }

        /* Start LVGL render loop AFTER all UI objects are created.
         * This prevents race conditions with e-paper's slow flush. */
        ret = ui_manager_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start LVGL render loop: %s",
                     esp_err_to_name(ret));
            return;
        }
#endif
    }

    /* Check for SD card firmware update */
    if (ota_sd_update_available()) {
        ESP_LOGI(TAG, "Firmware update detected on SD card!");
#ifdef THISTLE_LEGACY_C_APPS
        /* lvgl_task is running by now; post rather than show. */
        toast_post("Update available! Settings > About to install", TOAST_INFO, 5000);
#endif
    }

    /* Keep a newly flashed image rollback-eligible until all required boot
     * services have initialized and remained alive through a short soak. */
    ESP_LOGI(TAG, "Boot health milestone reached; soaking before OTA confirmation");
    vTaskDelay(pdMS_TO_TICKS(5000));
    ret = ota_mark_valid();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to confirm healthy OTA image: %s",
                 esp_err_to_name(ret));
        return;
    }
    ret = ota_finalize_bundle_transaction();
    if (ret != ESP_OK) {
        /* The image is already valid. Recovery will safely retry finalization
         * if the SD card was temporarily unavailable at this milestone. */
        ESP_LOGW(TAG, "Could not finalize recovery bundle transaction: %s",
                 esp_err_to_name(ret));
    }

    /* Say whether every driver started. The start sequence runs inside the
     * boot burst, where this board's only console drops lines, so a failure
     * there is otherwise invisible — and a driver that did not start is the
     * first thing to suspect when a peripheral is dead. */
    const char *failed_stage = NULL;
    esp_err_t start_failure = hal_registry_last_start_failure(&failed_stage);
    if (start_failure != ESP_OK) {
        ESP_LOGW(TAG, "Driver start: %s failed: %s",
                 failed_stage ? failed_stage : "unknown",
                 esp_err_to_name(start_failure));
    }

    /* Report the battery once the system is up, through whatever power driver
     * this board registered. It belongs here rather than in a driver's init:
     * bring-up logging happens in a burst the console drops lines from, and
     * this milestone is the first quiet moment after it. A board with no power
     * interface simply says nothing. */
    const hal_registry_t *registry = hal_get_registry();
    if (registry && registry->power && registry->power->get_info) {
        hal_power_info_t battery;
        esp_err_t battery_ret = registry->power->get_info(&battery);
        if (battery_ret == ESP_OK) {
            static const char *const STATE_NAMES[] = {
                "discharging", "charging", "charged", "no battery",
            };
            const char *state =
                battery.state < (sizeof(STATE_NAMES) / sizeof(STATE_NAMES[0]))
                    ? STATE_NAMES[battery.state]
                    : "unknown";
            ESP_LOGI(TAG, "Battery: %u mV, %u%%, %s (%s)",
                     battery.voltage_mv, battery.percent, state,
                     registry->power->name ? registry->power->name : "unnamed");
        } else if (battery_ret == ESP_ERR_INVALID_STATE && registry->power->init) {
            /* The driver is registered but not up. Its one chance to initialise
             * was inside the boot burst, when peripheral rails behind an IO
             * expander may still have been settling. Give it one more, now that
             * the board is quiet, and say what happened either way — this is
             * the only report a board with no console attached ever gives. */
            esp_err_t retry = registry->power->init(registry->power_config);
            if (retry == ESP_OK && registry->power->get_info(&battery) == ESP_OK) {
                ESP_LOGI(TAG, "Battery: %u mV, %u%% (%s, up on retry)",
                         battery.voltage_mv, battery.percent,
                         registry->power->name ? registry->power->name : "unnamed");
            } else {
                ESP_LOGW(TAG, "Battery: %s did not come up: %s",
                         registry->power->name ? registry->power->name : "unnamed",
                         esp_err_to_name(retry));
            }
        } else {
            ESP_LOGW(TAG, "Battery: %s could not be read: %s",
                     registry->power->name ? registry->power->name : "unnamed",
                     esp_err_to_name(battery_ret));
        }
    }

    /* Sleep by the clock and by the power key from here on. Not before:
     * waking from deep sleep restarts the device, and a restart before
     * ota_mark_valid() above would roll a new image back. */
    power_manager_start();

    /* Internal RAM is what runs out, so every boot says what is left of it,
     * this task's 16 KB stack still counted. */
    ESP_LOGI(TAG, "ThistleOS ready; internal RAM %u KB free, largest block %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));

    /* Returning ends this task and frees its stack, which only booting
     * needed: from here on everything runs on tasks of its own or as jobs on
     * the kernel worker (thistle/kworker.h). */
}
