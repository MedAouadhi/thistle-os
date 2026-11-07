// SPDX-License-Identifier: BSD-3-Clause
#pragma once

/*
 * What the Home screen decides without LVGL (home_core.c): where each band
 * goes on a screen of a given size, which apps are pinned, the date as Home
 * writes it, and the rows of what is new. launcher_ui.c draws what these say.
 *
 * Nothing here knows LVGL, the kernel or the time, so it is tested on the
 * host (simulator/tests/test_home_core.c).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The most apps Home pins: four columns of two rows. */
#define HOME_MAX_PINS 8

/* Left and right of the All apps button and the rows of what is new. */
#define HOME_MARGIN 8

/* The longest app ID Home keeps for a pinned place, NUL included, as a
 * manifest's. */
#define HOME_PIN_ID_MAX 64

/* Home, top to bottom, in the app area below the status bar. A band with no
 * room on the screen has a height of 0. The bar, when asked for, always has
 * its place. */
typedef struct {
    bool    landscape;        /* wider than tall: the date goes beside the time */
    int16_t clock_y, clock_h;
    int16_t now_y, now_h;     /* what is new: HOME_NOW_ROWS rows */
    int16_t grid_y, grid_h;   /* the pinned apps, `rows` of `cols` */
    uint8_t cols, rows;
    int16_t cell_w, cell_h;
    int16_t bar_y, bar_h;     /* the All apps button: 0 high without one */
} home_layout_t;

/* The bands for an app area `width` by `height`, with an All apps button
 * along the bottom when `apps_bar`: a board with an apps key
 * (HAL_KEY_APPS) opens All apps from it, and its pinned apps have the room. */
void home_layout(int16_t width, int16_t height, bool apps_bar, home_layout_t *out);

/* The apps to pin, at most `max`: those of `preferred` that are installed,
 * in that order, then the other installed apps `eligible` accepts, in the
 * order `installed` lists them. Writes indexes into `installed` to `out` and
 * returns how many. `eligible` may be NULL, to accept every app. */
int home_pick_pins(const char *const *preferred, size_t n_preferred,
                   const char *const *installed, size_t n_installed,
                   bool (*eligible)(const char *id),
                   int *out, int max);

/* The apps to pin, place by place, at most `max` places: in place i the
 * installed app `chosen[i]` names, if it does and it is not in an earlier
 * place; each place left over, in order, from home_pick_pins()'s choice of
 * the rest. Writes indexes into `installed` to `out`, -1 for a place with
 * nothing to put in it, and returns how many places there are: past the last
 * filled one, there are none. `chosen` may be NULL, and its entries NULL or
 * "" for a place the user never chose. */
int home_place_pins(const char *const *chosen, size_t n_chosen,
                    const char *const *preferred, size_t n_preferred,
                    const char *const *installed, size_t n_installed,
                    bool (*eligible)(const char *id),
                    int *out, int max);

/* Put `app_id` in place `slot` of the `n` places in `ids`, the app there
 * before going to the place `app_id` leaves, if it was pinned already: an
 * app is pinned once. Nothing for a slot out of range or an ID too long. */
void home_choose_pin(char ids[][HOME_PIN_ID_MAX], int n, int slot, const char *app_id);

/* The date as Home shows it, "Friday 16 October", or with the weekday on a
 * line of its own when `two_lines`. `wday` 0 is Sunday and `mon` 0 January,
 * as in struct tm; anything out of range writes "". */
void home_format_date(int wday, int mday, int mon, bool two_lines,
                      char *buf, size_t len);

/* ------------------------------------------------------------------ */
/* What is new                                                          */
/* ------------------------------------------------------------------ */

/* The most rows of what is new Home shows. */
#define HOME_NOW_ROWS 2

#define HOME_NOW_TITLE_MAX  32
#define HOME_NOW_DETAIL_MAX 96
#define HOME_NOW_NUMBER_MAX 33   /* as CALL_NUMBER_MAX */

/* A fixed set, in the order they are shown: the first two that apply, or
 * with none of the first four, CAUGHT_UP and then MESH. SYSTEM_UPDATE and
 * APP_UPDATES are one row, the one that applies. */
typedef enum {
    HOME_NOW_MESSAGES,     /* unread SMS, mesh and Tailnet chat messages: opens
                            * the app home_now_messages_app() names */
    HOME_NOW_MISSED_CALL,  /* missed calls: opens Phone; its button calls back */
    HOME_NOW_TAILNET,      /* the tailnet waits for the user: opens Tailscale */
    HOME_NOW_SYSTEM_UPDATE,/* ThistleOS has an update, and perhaps apps: opens
                            * Settings -> System update */
    HOME_NOW_APP_UPDATES,  /* only apps have updates: opens the App Store */
    HOME_NOW_CAUGHT_UP,    /* nothing new: not a button */
    HOME_NOW_MESH,         /* the mesh is on: opens MeshCore */
} home_now_kind_t;

/* What the tailnet asks of the user, if anything. */
typedef enum {
    HOME_TAILNET_QUIET,     /* nothing: off, connected, starting, ... */
    HOME_TAILNET_LOGIN,     /* sign in to join */
    HOME_TAILNET_APPROVAL,  /* signed in; an admin must approve the device */
} home_tailnet_t;

/* What Home reads from the kernel. */
typedef struct {
    uint32_t sms_unread;
    uint32_t mesh_unread;
    uint32_t chat_unread;   /* Tailnet chat's, tailnet_chat_unread_count() */
    uint32_t missed_calls;
    /* The newest missed call: its number, "" when withheld, NULL when the
     * call log has none; the phonebook's name for it, NULL or "" for none;
     * and when it was, in local time, missed_hour -1 when the clock was not
     * set. */
    const char *missed_number;
    const char *missed_name;
    int8_t   missed_hour;
    int8_t   missed_minute;
    bool     format_24h;
    home_tailnet_t tailnet;
    bool     mesh_running;
    int      mesh_contacts;
    /* The ThistleOS version on offer, NULL or "" for none; and how many
     * installed apps have an update. */
    const char *os_update;
    uint32_t app_updates;
} home_now_in_t;

typedef struct {
    home_now_kind_t kind;
    char title[HOME_NOW_TITLE_MAX];
    char detail[HOME_NOW_DETAIL_MAX];
    /* The number the row's button calls, "" for a row with no button. */
    char call_back[HOME_NOW_NUMBER_MAX];
} home_now_row_t;

/* The rows of what is new for `in`, at most `max`: writes them to `out`,
 * zeroed first so two answers compare with memcmp(), and returns how many.
 * Counts only, never what a message says or who sent it. */
int home_now_rows(const home_now_in_t *in, home_now_row_t *out, int max);

/* The app holding unread messages, which the messages row and the status
 * bar's envelope open: the only one with any, else the first of SMS,
 * MeshCore and Chat that has some, as the row's detail lists them. SMS when
 * none has any. */
typedef enum {
    HOME_MSG_SMS,
    HOME_MSG_MESH,
    HOME_MSG_CHAT,
} home_msg_app_t;
home_msg_app_t home_now_messages_app(const home_now_in_t *in);

/* That app's ID. */
const char *home_msg_app_id(home_msg_app_t app);
