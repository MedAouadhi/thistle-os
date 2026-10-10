/*
 * ThistleOS Simulator — SDL2 host application
 *
 * Runs the real ThistleOS UI in an SDL2 window for development/testing.
 * Pass --device <name> to simulate a specific hardware target.
 * Default device: tdeck (320x240, 2x scale).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>

static pthread_mutex_t s_lvgl_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Public accessor for drivers/tasks that need to touch LVGL */
void sim_lvgl_lock(void)   { pthread_mutex_lock(&s_lvgl_mutex); }
void sim_lvgl_unlock(void) { pthread_mutex_unlock(&s_lvgl_mutex); }

#include "lvgl.h"
#include "hal/board.h"
#include "thistle/kernel.h"
#include "thistle/app_manager.h"
#include "thistle/display_server.h"
#include "ui/manager.h"
#include "thistle/board_config.h"
#include "ui/lvgl_wm.h"
#include "sim_input.h"
#include "sim_display.h"
#include "sim_vfs.h"
#include "sim_assert.h"
#include "sim_scenario.h"
#include "sim_app_loader.h"
#include "lvgl_apps.h"

static bool s_headless = false;
static int  s_timeout_ms = 0;
static const char *s_assert_file = NULL;
static const char *s_scenario_file = NULL;
static const char *s_screenshot_file = NULL;
static const char *s_launch_app = NULL;
/* Overrides system.json's "window_manager" for this run only. The simulator
 * shares one sdcard directory between runs, so a test that needs a manager
 * other than the board's default cannot get it by editing that file. */
static const char *s_wm_override = NULL;
/* Taps to inject after startup, in order, SIM_TAP_GAP_MS apart. Each is
 * held for s_tap_hold_ms; 0 delivers the press and the release before LVGL
 * next reads the pointer, as a short tap does on hardware whose UI task is
 * busy refreshing a panel. */
#define SIM_MAX_TAPS   8
#define SIM_TAP_GAP_MS 1500u
static struct { int x, y; } s_taps[SIM_MAX_TAPS];
static int s_tap_count = 0;
static int s_tap_hold_ms = 100;

static bool add_tap(const char *spec)
{
    if (s_tap_count >= SIM_MAX_TAPS ||
        sscanf(spec, "%d,%d", &s_taps[s_tap_count].x, &s_taps[s_tap_count].y) != 2) {
        fprintf(stderr, "Invalid --tap value; expected X,Y (at most %d taps)\n",
                SIM_MAX_TAPS);
        return false;
    }
    s_tap_count++;
    return true;
}

bool sim_is_headless(void) { return s_headless; }

static void sim_tk_input_cb(const hal_input_event_t *event, void *user_data)
{
    (void)user_data;
    const display_server_wm_t *wm = thistle_tk_wm_get();
    if (wm && wm->on_input) wm->on_input(event);
}

/* Defined in board_simulator.c */
extern void sim_board_set_device(const char *device);

int main(int argc, char **argv)
{
    const char *device = "tdeck";  /* default device */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strncmp(argv[i], "--device=", 9) == 0) {
            device = argv[i] + 9;
        } else if (strcmp(argv[i], "--headless") == 0) {
            s_headless = true;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            s_timeout_ms = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--timeout=", 10) == 0) {
            s_timeout_ms = atoi(argv[i] + 10);
        } else if (strcmp(argv[i], "--assert") == 0 && i + 1 < argc) {
            s_assert_file = argv[++i];
        } else if (strncmp(argv[i], "--assert=", 9) == 0) {
            s_assert_file = argv[i] + 9;
        } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            s_scenario_file = argv[++i];
        } else if (strncmp(argv[i], "--scenario=", 11) == 0) {
            s_scenario_file = argv[i] + 11;
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            s_screenshot_file = argv[++i];
        } else if (strncmp(argv[i], "--screenshot=", 13) == 0) {
            s_screenshot_file = argv[i] + 13;
        } else if (strcmp(argv[i], "--tap") == 0 && i + 1 < argc) {
            if (!add_tap(argv[++i])) return 2;
        } else if (strcmp(argv[i], "--tap-hold") == 0 && i + 1 < argc) {
            s_tap_hold_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--launch-app") == 0 && i + 1 < argc) {
            s_launch_app = argv[++i];
        } else if (strncmp(argv[i], "--launch-app=", 13) == 0) {
            s_launch_app = argv[i] + 13;
        } else if (strcmp(argv[i], "--wm") == 0 && i + 1 < argc) {
            s_wm_override = argv[++i];
        } else if (strncmp(argv[i], "--wm=", 5) == 0) {
            s_wm_override = argv[i] + 5;
        } else if (strncmp(argv[i], "--tap=", 6) == 0) {
            if (!add_tap(argv[i] + 6)) return 2;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [OPTIONS]\n", argv[0]);
            printf("Options:\n");
            printf("  --device NAME       Simulate a specific board (default: tdeck)\n");
            printf("  --headless          Run without SDL window (framebuffer only)\n");
            printf("  --timeout MS        Exit after MS milliseconds (headless mode)\n");
            printf("  --assert FILE       Evaluate assertions from FILE on exit\n");
            printf("  --scenario FILE     Replay input scenario from FILE (future)\n");
            printf("  --screenshot FILE   Save native-resolution framebuffer as PPM on exit\n");
            printf("  --tap X,Y           Inject a native-coordinate tap after startup;\n");
            printf("                      repeat for more, %u ms apart\n", SIM_TAP_GAP_MS);
            printf("  --tap-hold MS       How long each tap is held (default 100;\n");
            printf("                      0 = press and release between two pointer reads)\n");
            printf("  --launch-app ID     Launch a registered app instead of the launcher\n");
            printf("  --wm NAME           Window manager, overriding system.json:\n");
            printf("                      thistle-tk, lvgl-epaper-wm or lvgl-lcd-wm\n");
            printf("  -h, --help          Show this help\n");
            printf("Devices: tdeck-pro, tdeck, tdeck-plus, tdeck-max, tdisplay,\n");
            printf("         heltec-v3, cardputer, t3-s3, rak3312\n");
            return 0;
        }
    }

    sim_board_set_device(device);
    printf("ThistleOS Simulator — %s\n", device);
    fflush(stdout);

    /* Set up simulated SD card filesystem (symlink to simulator/sdcard/) */
    sim_vfs_init();

    if (s_assert_file) {
        sim_assert_init(s_assert_file);
    }

    if (s_scenario_file) {
        sim_scenario_load(s_scenario_file);
    }

    /* Initialize kernel (board + drivers + event bus + IPC + syscalls + apps) */
    esp_err_t err = kernel_init();
    { char _msg[64]; snprintf(_msg, sizeof(_msg), "kernel_init: %d", err);
      printf("%s\n", _msg); sim_assert_check_line(_msg); }
    fflush(stdout);

    /* Initialize display server and register the LVGL window manager */
    err = display_server_init();
    { char _msg[64]; snprintf(_msg, sizeof(_msg), "display_server_init: %d", err);
      printf("%s\n", _msg); sim_assert_check_line(_msg); }
    fflush(stdout);

    extern bool sim_board_is_epaper(void);
    /* Same selection as the firmware, from the same system.json key, so a
     * board configured here behaves the way it will on hardware. */
    thistle_wm_choice_t choice =
        thistle_wm_select(s_wm_override ? s_wm_override : board_config_get_wm_name(),
                          sim_board_is_epaper());
    bool use_tk_wm = !choice.hosts_lvgl_apps;
    err = display_server_register_wm(choice.wm);
    { char _msg[64]; snprintf(_msg, sizeof(_msg), "display_server_register_wm: %d", err);
      printf("%s\n", _msg); sim_assert_check_line(_msg); }
    fflush(stdout);

    if (use_tk_wm) {
        const hal_input_driver_t *input = sim_input_get();
        if (input && input->register_callback) {
            input->register_callback(sim_tk_input_cb, NULL);
        }
    }

    /* Signed host TAP generations are registered before legacy built-ins so
     * an installed app wins the idempotent duplicate-ID registration rule. */
    sim_app_loader_scan_and_register();

    /* LVGL built-in apps; see lvgl_apps.h for why they need an LVGL window
     * manager. sim_app_loader_scan_and_register() runs above this and is
     * unaffected, so a signed .tap app still registers under thistle-tk —
     * which is what test_tap_notes.py installs and taps. */
    extern bool sim_board_has_radio(void);
    extern bool sim_board_has_gps(void);
    if (!use_tk_wm) {
        err = lvgl_apps_register_builtin(sim_board_has_radio(),
                                         sim_board_has_gps());
        if (err != ESP_OK) {
            fprintf(stderr, "Some built-in apps failed to register: %d\n", err);
        }
    } else {
        /* thistle-tk's own App Store and Flashlight; they share IDs with the
         * LVGL ones, so each manager registers only its own. */
        err = tk_native_apps_register();
        if (err != ESP_OK) {
            fprintf(stderr, "thistle-tk apps failed to register: %d\n", err);
        }
    }

    const char *initial_app = s_launch_app
        ? s_launch_app
        : (use_tk_wm ? "com.thistle.tk_launcher" : "com.thistle.launcher");
    err = app_manager_launch(initial_app);
    if (err != ESP_OK) {
        fprintf(stderr, "Failed to launch app %s: %d\n", initial_app, err);
        return 1;
    }
    if (s_launch_app) {
        printf("App launched: %s\n", initial_app);
    } else {
        printf("Launcher launched\n");
        sim_assert_check_line("Launcher launched");
    }
    fflush(stdout);

    printf("ThistleOS Simulator ready. Close window to exit.\n");
    sim_assert_check_line("ThistleOS Simulator ready");
    fflush(stdout);

    /* Main loop — drive LVGL tick + timer handler + SDL event pump */
    uint32_t last_tick = 0;
    uint32_t start_ms = 0;
    int tap_index = 0;
    int tap_phase = 0;   /* 0 = waiting to press, 1 = pressed */
    while (1) {
        /* Update LVGL tick */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint32_t now_ms = (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
        if (last_tick == 0) last_tick = now_ms;
        uint32_t elapsed = now_ms - last_tick;
        if (start_ms == 0) start_ms = now_ms;
        if (elapsed > 0) {
            lv_tick_inc(elapsed);
            last_tick = now_ms;
        }

        /* Pump SDL events → HAL input events */
        sim_input_poll_sdl();

        if (use_tk_wm) {
            display_server_tick();
        } else {
            /* Run LVGL timer handler (renders, processes animations) */
            pthread_mutex_lock(&s_lvgl_mutex);
            lv_timer_handler();
            pthread_mutex_unlock(&s_lvgl_mutex);
        }

        if (tap_index < s_tap_count) {
            uint16_t tx = (uint16_t)s_taps[tap_index].x;
            uint16_t ty = (uint16_t)s_taps[tap_index].y;
            uint32_t tap_down_ms = (use_tk_wm ? 250u : 2250u) +
                                   (uint32_t)tap_index * SIM_TAP_GAP_MS;
            uint32_t tap_up_ms = tap_down_ms + (uint32_t)s_tap_hold_ms;
            uint32_t since_start = now_ms - start_ms;
            if (tap_phase == 0 && since_start > tap_down_ms &&
                sim_input_inject_touch(tx, ty, true)) {
                tap_phase = 1;
            }
            /* Same iteration when the hold is 0: LVGL never runs in between. */
            if (tap_phase == 1 && since_start >= tap_up_ms &&
                sim_input_inject_touch(tx, ty, false)) {
                tap_phase = 0;
                tap_index++;
            }
        }

        /* Headless timeout */
        if (s_headless && s_timeout_ms > 0 && (now_ms - start_ms) > (uint32_t)s_timeout_ms) {
            printf("Simulator timeout reached (%d ms)\n", s_timeout_ms);
            if (s_screenshot_file && !sim_display_save_ppm(s_screenshot_file)) {
                fprintf(stderr, "Failed to save simulator screenshot\n");
                exit(1);
            }
            int rc = s_assert_file ? sim_assert_evaluate() : 0;
            exit(rc);
        }

        usleep(5000);  /* ~200 fps cap */
    }

    return 0;
}
