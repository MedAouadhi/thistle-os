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
/* keycode is HAL_KEY_HOME, HAL_KEY_BACK or HAL_KEY_QUICK for a press of that
 * key in place of a tap, and 0 for a tap at x,y. A drag goes down at x,y and
 * moves to x2,y2 over SIM_DRAG_MS before it lifts. */
static struct { int x, y, x2, y2; uint16_t keycode; bool drag; } s_taps[SIM_MAX_TAPS];
static int s_tap_count = 0;
static int s_tap_hold_ms = 100;
#define SIM_DRAG_MS 300u
/* Text to type after the taps. Every key's press and release arrive in one
 * main-loop iteration, as one drain of the TCA8418's event FIFO delivers
 * them, so LVGL reads the keypad only after all of them. */
static const char *s_type_text = NULL;
/* Press each key before releasing the one before it, as fast typing does. */
static bool s_type_rollover = false;
/* Messages the mesh hears at startup, before any app opens, as "SENDER: TEXT".
 * The simulator has no radio; these reach the kernel's inbox as the engine's
 * would on a device. */
#define SIM_MAX_MESH_MESSAGES 8
static struct { const char *spec; bool direct; } s_mesh_messages[SIM_MAX_MESH_MESSAGES];
static int s_mesh_message_count = 0;

extern bool rs_mesh_sim_hear(const char *sender, const char *text, bool direct);

static bool add_mesh_message(const char *spec, bool direct)
{
    if (s_mesh_message_count >= SIM_MAX_MESH_MESSAGES || strchr(spec, ':') == NULL) {
        fprintf(stderr, "Invalid mesh message; expected \"SENDER: TEXT\" (at most %d)\n",
                SIM_MAX_MESH_MESSAGES);
        return false;
    }
    s_mesh_messages[s_mesh_message_count].spec = spec;
    s_mesh_messages[s_mesh_message_count].direct = direct;
    s_mesh_message_count++;
    return true;
}

static void hear_mesh_messages(void)
{
    for (int i = 0; i < s_mesh_message_count; i++) {
        const char *spec = s_mesh_messages[i].spec;
        const char *colon = strchr(spec, ':');
        char sender[32];
        size_t n = (size_t)(colon - spec);
        if (n >= sizeof(sender)) n = sizeof(sender) - 1;
        memcpy(sender, spec, n);
        sender[n] = '\0';
        const char *text = colon + 1;
        while (*text == ' ') text++;
        rs_mesh_sim_hear(sender, text, s_mesh_messages[i].direct);
    }
    if (s_mesh_message_count > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Mesh heard: %d messages", s_mesh_message_count);
        printf("%s\n", msg);
        sim_assert_check_line(msg);
    }
}

static bool add_tap(const char *spec)
{
    if (s_tap_count < SIM_MAX_TAPS) {
        s_taps[s_tap_count].drag = false;
        s_taps[s_tap_count].keycode = strcmp(spec, "home") == 0  ? HAL_KEY_HOME
                                    : strcmp(spec, "back") == 0  ? HAL_KEY_BACK
                                    : strcmp(spec, "quick") == 0 ? HAL_KEY_QUICK : 0;
    }
    if (s_tap_count >= SIM_MAX_TAPS ||
        (s_taps[s_tap_count].keycode == 0 &&
         sscanf(spec, "%d,%d", &s_taps[s_tap_count].x, &s_taps[s_tap_count].y) != 2)) {
        fprintf(stderr, "Invalid --tap value; expected X,Y, home, back or quick "
                        "(at most %d taps)\n", SIM_MAX_TAPS);
        return false;
    }
    s_tap_count++;
    return true;
}

/* A drag, taking its turn among the taps. */
static bool add_drag(const char *spec)
{
    if (s_tap_count >= SIM_MAX_TAPS ||
        sscanf(spec, "%d,%d,%d,%d", &s_taps[s_tap_count].x, &s_taps[s_tap_count].y,
               &s_taps[s_tap_count].x2, &s_taps[s_tap_count].y2) != 4) {
        fprintf(stderr, "Invalid --drag value; expected X1,Y1,X2,Y2 (at most %d taps "
                        "and drags)\n", SIM_MAX_TAPS);
        return false;
    }
    s_taps[s_tap_count].keycode = 0;
    s_taps[s_tap_count].drag = true;
    s_tap_count++;
    return true;
}

bool sim_is_headless(void) { return s_headless; }

/* Report every visible object that reaches past the left or right edge of the
 * display. On the panel that is text and borders nobody can see: a 280 px
 * dialog centred on a 240 px screen lost the first letter of each line and the
 * side borders of its buttons. Only the horizontal is checked, because a list
 * that scrolls is meant to run off the bottom. */
static bool s_report_offscreen = false;

static const char *first_label_text(lv_obj_t *obj)
{
    if (lv_obj_check_type(obj, &lv_label_class)) return lv_label_get_text(obj);
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        const char *text = first_label_text(lv_obj_get_child(obj, (int32_t)i));
        if (text) return text;
    }
    return NULL;
}

static int report_offscreen(lv_obj_t *obj, int32_t display_w)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return 0;

    int count = 0;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    if (a.x1 < 0 || a.x2 >= display_w) {
        const char *text = first_label_text(obj);
        printf("Off screen: x %d..%d of %d, \"%s\"\n",
               (int)a.x1, (int)a.x2, (int)display_w, text ? text : "");
        count++;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        count += report_offscreen(lv_obj_get_child(obj, (int32_t)i), display_w);
    }
    return count;
}

static void sim_tk_input_cb(const hal_input_event_t *event, void *user_data)
{
    (void)user_data;
    const display_server_wm_t *wm = thistle_tk_wm_get();
    if (wm && wm->on_input) wm->on_input(event);
}

/* Defined in board_simulator.c */
extern void sim_board_set_device(const char *device);
extern void sim_cellular_set_card(const char *card);
extern bool sim_cellular_add_sms(const char *spec);

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
        } else if (strcmp(argv[i], "--drag") == 0 && i + 1 < argc) {
            if (!add_drag(argv[++i])) return 2;
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
        } else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
            s_type_text = argv[++i];
        } else if (strcmp(argv[i], "--type-rollover") == 0) {
            s_type_rollover = true;
        } else if (strcmp(argv[i], "--mesh-message") == 0 && i + 1 < argc) {
            if (!add_mesh_message(argv[++i], false)) return 2;
        } else if (strcmp(argv[i], "--mesh-direct") == 0 && i + 1 < argc) {
            if (!add_mesh_message(argv[++i], true)) return 2;
        } else if (strcmp(argv[i], "--sim-card") == 0 && i + 1 < argc) {
            sim_cellular_set_card(argv[++i]);
        } else if (strcmp(argv[i], "--sms") == 0 && i + 1 < argc) {
            if (!sim_cellular_add_sms(argv[++i])) {
                fprintf(stderr, "Invalid SMS; expected \"SENDER: TEXT\" (at most 8)\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--log-flushes") == 0) {
            sim_display_set_log_flushes(true);
        } else if (strcmp(argv[i], "--report-offscreen") == 0) {
            s_report_offscreen = true;
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
            printf("  --tap home|back|quick  Press the Home, Back or quick key instead\n");
            printf("  --drag X1,Y1,X2,Y2  Drag from X1,Y1 to X2,Y2 over %u ms, in turn\n"
                   "                      with the taps\n", SIM_DRAG_MS);
            printf("  --tap-hold MS       How long each tap is held (default 100;\n");
            printf("                      0 = press and release between two pointer reads)\n");
            printf("  --type TEXT         Type TEXT after the taps, every key pressed and\n");
            printf("                      released before LVGL next reads the keypad;\n");
            printf("                      prints the focused text field on exit\n");
            printf("  --type-rollover     Press each typed key before releasing the last\n");
            printf("  --sim-card STATE    The modem's SIM, on a device with one (tdeck-max):\n");
            printf("                      ready (default), absent, pin or puk\n");
            printf("  --sms \"SENDER: TEXT\" An SMS the modem holds at startup, for the\n");
            printf("                      kernel to fetch once it registers; up to 8\n");
            printf("  --mesh-message \"SENDER: TEXT\"\n");
            printf("                      Hear TEXT on the mesh's Public channel at startup\n");
            printf("  --mesh-direct \"SENDER: TEXT\"\n");
            printf("                      Hear TEXT as a direct message from SENDER; up to\n");
            printf("                      %d messages of either kind\n", SIM_MAX_MESH_MESSAGES);
            printf("  --log-flushes       Print every area flushed to the display\n");
            printf("  --report-offscreen  On exit, list every visible LVGL object that\n");
            printf("                      reaches past the left or right screen edge\n");
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
    hear_mesh_messages();
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
    bool typed = false;
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
            uint16_t key = s_taps[tap_index].keycode;
            uint32_t tap_down_ms = (use_tk_wm ? 250u : 2250u) +
                                   (uint32_t)tap_index * SIM_TAP_GAP_MS;
            uint32_t tap_up_ms = tap_down_ms + (uint32_t)s_tap_hold_ms;
            uint32_t since_start = now_ms - start_ms;
            if (tap_phase == 0 && since_start > tap_down_ms &&
                (key ? sim_input_inject_key(key, true) : sim_input_inject_touch(tx, ty, true))) {
                tap_phase = 1;
            }
            if (tap_phase == 1 && s_taps[tap_index].drag) {
                /* Along the line, as far as the time says, a move per loop. */
                uint32_t t = since_start - tap_down_ms;
                if (t > SIM_DRAG_MS) t = SIM_DRAG_MS;
                int x = s_taps[tap_index].x + (s_taps[tap_index].x2 - s_taps[tap_index].x) *
                                                  (int)t / (int)SIM_DRAG_MS;
                int y = s_taps[tap_index].y + (s_taps[tap_index].y2 - s_taps[tap_index].y) *
                                                  (int)t / (int)SIM_DRAG_MS;
                sim_input_inject_touch_move((uint16_t)x, (uint16_t)y);
                tap_up_ms = tap_down_ms + SIM_DRAG_MS;
                tx = (uint16_t)s_taps[tap_index].x2;
                ty = (uint16_t)s_taps[tap_index].y2;
            }
            /* Same iteration when the hold is 0: LVGL never runs in between. */
            if (tap_phase == 1 && since_start >= tap_up_ms &&
                (key ? sim_input_inject_key(key, false) : sim_input_inject_touch(tx, ty, false))) {
                tap_phase = 0;
                tap_index++;
            }
        }

        if (s_type_text && !typed && tap_index >= s_tap_count) {
            uint32_t type_ms = (use_tk_wm ? 250u : 2250u) +
                               (uint32_t)s_tap_count * SIM_TAP_GAP_MS;
            if (now_ms - start_ms > type_ms) {
                for (const char *c = s_type_text; *c; c++) {
                    sim_input_inject_key((uint8_t)*c, true);
                    if (!s_type_rollover) {
                        sim_input_inject_key((uint8_t)*c, false);
                    } else if (c > s_type_text) {
                        sim_input_inject_key((uint8_t)c[-1], false);
                    }
                }
                size_t n = strlen(s_type_text);
                if (s_type_rollover && n > 0) {
                    sim_input_inject_key((uint8_t)s_type_text[n - 1], false);
                }
                typed = true;
            }
        }

        /* Headless timeout */
        if (s_headless && s_timeout_ms > 0 && (now_ms - start_ms) > (uint32_t)s_timeout_ms) {
            printf("Simulator timeout reached (%d ms)\n", s_timeout_ms);
            if (s_type_text && !use_tk_wm) {
                lv_obj_t *focused = lv_group_get_focused(lv_group_get_default());
                printf("Focused text: \"%s\"\n",
                       focused && lv_obj_check_type(focused, &lv_textarea_class)
                           ? lv_textarea_get_text(focused) : "");
            }
            if (s_report_offscreen && !use_tk_wm) {
                int32_t display_w = lv_display_get_horizontal_resolution(NULL);
                int count = report_offscreen(lv_screen_active(), display_w) +
                            report_offscreen(lv_layer_top(), display_w);
                printf("Off-screen objects: %d\n", count);
            }
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
