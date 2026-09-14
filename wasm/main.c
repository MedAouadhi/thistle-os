/*
 * ThistleOS WASM Simulator — browser entry point
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mirrors simulator/main.c but uses Emscripten's main loop instead of
 * a blocking while(1). SDL2 is auto-shimmed to HTML5 Canvas.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <emscripten.h>

#include "lvgl.h"
#include "hal/board.h"
#include "thistle/kernel.h"
#include "thistle/app_manager.h"
#include "thistle/display_server.h"
#include "ui/lvgl_wm.h"
#include "sim_input.h"
#include "sim_vfs.h"

/* App registration headers — must match simulator/main.c */
#if THISTLE_HAVE_LAUNCHER
#include "launcher/launcher_app.h"
#endif
#if THISTLE_HAVE_SETTINGS
#include "settings/settings_app.h"
#endif
#if THISTLE_HAVE_FILEMGR
#include "file_manager/filemgr_app.h"
#endif
#if THISTLE_HAVE_READER
#include "reader/reader_app.h"
#endif
#if THISTLE_HAVE_MESSENGER
#include "messenger/messenger_app.h"
#endif
#if THISTLE_HAVE_NAVIGATOR
#include "navigator/navigator_app.h"
#endif
#if THISTLE_HAVE_NOTES
#include "notes/notes_app.h"
#endif
#if THISTLE_HAVE_APPSTORE
#include "appstore/appstore_app.h"
#endif
#if THISTLE_HAVE_ASSISTANT
#include "assistant/assistant_app.h"
#endif
#if THISTLE_HAVE_WIFISCANNER
#include "wifiscanner/wifiscanner_app.h"
#endif
#if THISTLE_HAVE_FLASHLIGHT
#include "flashlight/flashlight_app.h"
#endif
#if THISTLE_HAVE_WEATHER
#include "weather/weather_app.h"
#endif
#if THISTLE_HAVE_TERMINAL
#include "terminal/terminal_app.h"
#endif
#if THISTLE_HAVE_VAULT
#include "vault/vault_app.h"
#endif

/* Defined in board_simulator.c (shared with SDL sim) */
extern void sim_board_set_device(const char *device);
extern bool sim_board_has_radio(void);
extern bool sim_board_has_gps(void);

/* ------------------------------------------------------------------ */
/* Device selection — JS calls _wasm_set_device(idx) before main      */
/* ------------------------------------------------------------------ */

/* Defined in simulator/board_simulator.c */
extern bool sim_board_is_epaper(void);

/* thistle-tk draws its own widgets and hands the display server a packed
 * 1-bit framebuffer, which is what sim_display unpacks for an e-paper
 * device. It does not use LVGL's input device layer, so input is forwarded
 * to it by hand. Mirrors simulator/main.c. */
static void wasm_tk_input_cb(const hal_input_event_t *event, void *user_data)
{
    (void)user_data;
    const display_server_wm_t *wm = thistle_tk_wm_get();
    if (wm && wm->on_input) wm->on_input(event);
}

/* Order must match the devs array in wasm/shell.html — JS sends the index. */
static const char *DEVICE_NAMES[] = {
    "tdeck", "tdeck-pro", "tdeck-plus", "tdeck-max", "tdisplay",
    "heltec-v3", "cardputer", "t3-s3", "rak3312"
};
#define NUM_DEVICES ((int)(sizeof(DEVICE_NAMES) / sizeof(DEVICE_NAMES[0])))
static const char *wasm_device_name = "tdeck";

EMSCRIPTEN_KEEPALIVE
void wasm_set_device(int idx) {
    if (idx >= 0 && idx < NUM_DEVICES) {
        wasm_device_name = DEVICE_NAMES[idx];
    }
}

/* ------------------------------------------------------------------ */
/* Main loop callback — drives the window manager at ~60 FPS          */
/* ------------------------------------------------------------------ */

/* Set once the window manager is chosen; thistle-tk and LVGL are driven by
 * different calls and only one of them is running. */
static bool s_use_tk_wm = false;

static void main_loop(void)
{
    /* Poll HAL input drivers (SDL mouse/keyboard → HAL events) */
    sim_input_poll_sdl();

    lv_tick_inc(16);  /* ~60 FPS */
    if (s_use_tk_wm) {
        /* thistle-tk composites and flushes itself; it has no LVGL timers. */
        display_server_tick();
    } else {
        lv_timer_handler();
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("ThistleOS WASM Simulator — %s\n", wasm_device_name);
    sim_board_set_device(wasm_device_name);

    /* Set up simulated SD card filesystem */
    sim_vfs_init();

    /* Initialize kernel (board + drivers + event bus + IPC + syscalls) */
    int ret = kernel_init();
    printf("kernel_init: %d\n", ret);

    /* Initialize display server and register LVGL window manager */
    ret = display_server_init();
    printf("display_server_init: %d\n", ret);

    /* An e-paper device needs thistle-tk: sim_display unpacks its flushes as
     * a packed 1-bit framebuffer, and the LCD window manager emits RGB565,
     * which renders as noise. */
    s_use_tk_wm = sim_board_is_epaper();
    ret = display_server_register_wm(s_use_tk_wm ? thistle_tk_wm_get()
                                                 : lvgl_lcd_wm_get());
    printf("display_server_register_wm: %d\n", ret);

    if (s_use_tk_wm) {
        const hal_input_driver_t *input = sim_input_get();
        if (input && input->register_callback) {
            input->register_callback(wasm_tk_input_cb, NULL);
        }
    }

    /* Register built-in apps — same order as simulator/main.c.
     *
     * Only under an LVGL window manager: each of these parents its widgets to
     * ui_manager_get_app_area(), which is NULL until an LVGL window manager
     * runs ui_manager_init(). Launching one under thistle-tk dereferences that
     * NULL. main/main.c guards the same list the same way, so registering them
     * here unconditionally showed a launcher the device never shows. */
    if (!s_use_tk_wm) {
#if THISTLE_HAVE_LAUNCHER
        launcher_app_register();
#endif
#if THISTLE_HAVE_SETTINGS
        settings_app_register();
#endif
#if THISTLE_HAVE_FILEMGR
        filemgr_app_register();
#endif
#if THISTLE_HAVE_READER
        reader_app_register();
#endif
#if THISTLE_HAVE_NOTES
        notes_app_register();
#endif
#if THISTLE_HAVE_FLASHLIGHT
        flashlight_app_register();
#endif
#if THISTLE_HAVE_VAULT
        vault_app_register();
#endif
#if THISTLE_HAVE_APPSTORE
        appstore_app_register();
#endif
#if THISTLE_HAVE_TERMINAL
        terminal_app_register();
#endif
#if THISTLE_HAVE_ASSISTANT
        assistant_app_register();
#endif
#if THISTLE_HAVE_WEATHER
        weather_app_register();
#endif

        /* Conditional apps based on device capabilities */
        if (sim_board_has_radio()) {
#if THISTLE_HAVE_MESSENGER
            messenger_app_register();
#endif
#if THISTLE_HAVE_WIFISCANNER
            wifiscanner_app_register();
#endif
        }
        if (sim_board_has_gps()) {
#if THISTLE_HAVE_NAVIGATOR
            navigator_app_register();
#endif
        }
    }

    /* Each window manager has its own launcher: the LVGL one builds LVGL
     * widgets, which thistle-tk never renders. */
    app_manager_launch(s_use_tk_wm ? "com.thistle.tk_launcher"
                                   : "com.thistle.launcher");

    printf("ThistleOS WASM ready. Running main loop.\n");

    /* Emscripten main loop — renders at 60 FPS via requestAnimationFrame */
    emscripten_set_main_loop(main_loop, 0, 1);

    return 0;
}
