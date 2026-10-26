// SPDX-License-Identifier: BSD-3-Clause
/*
 * What the Home screen decides without LVGL: see home_core.h.
 */
#include "home_core.h"

#include <stdio.h>
#include <string.h>

#define BAR_H        36   /* a 28 px button, with 4 px above and below it */
#define ROW_H        56   /* a pinned app: its 36 px icon and its name */
#define CLOCK_H_TALL 64   /* the time, and the date under it */
#define CLOCK_H_WIDE 48   /* the time, and the date beside it */
#define NOW_H_TALL   80   /* two rows of 40 */
#define NOW_H_WIDE   64   /* two rows of 30, 4 apart */
#define MAX_ROWS      2

void home_layout(int16_t width, int16_t height, home_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    out->landscape = width > height;
    out->cols = out->landscape ? 4 : 3;
    out->bar_h = BAR_H;
    out->bar_y = height > BAR_H ? (int16_t)(height - BAR_H) : 0;

    int16_t y = 0;
    int16_t room = out->bar_y;
    const int16_t clock_h = out->landscape ? CLOCK_H_WIDE : CLOCK_H_TALL;
    const int16_t now_h = out->landscape ? NOW_H_WIDE : NOW_H_TALL;

    /* The clock only with a row of apps under it, and what is new only with
     * the clock above it and a row of apps under it: on a short screen the
     * apps come first. */
    if (room >= clock_h + ROW_H) {
        out->clock_y = y;
        out->clock_h = clock_h;
        y += clock_h;
        room -= clock_h;
        if (room >= now_h + ROW_H) {
            out->now_y = y;
            out->now_h = now_h;
            y += now_h;
            room -= now_h;
        }
    }

    int rows = room / ROW_H;
    out->rows = (uint8_t)(rows > MAX_ROWS ? MAX_ROWS : rows);
    out->grid_y = y;
    out->grid_h = room;
    out->cell_h = ROW_H;
    /* The whole width, margins and all: at 240 px a cell of 80 holds
     * "Messenger", one inside the margins does not. */
    out->cell_w = (int16_t)(width > 0 ? width / out->cols : 0);
}

static bool picked(const int *out, int n, int index)
{
    for (int i = 0; i < n; i++) {
        if (out[i] == index) {
            return true;
        }
    }
    return false;
}

int home_pick_pins(const char *const *preferred, size_t n_preferred,
                   const char *const *installed, size_t n_installed,
                   bool (*eligible)(const char *id),
                   int *out, int max)
{
    int n = 0;
    for (size_t p = 0; p < n_preferred && n < max; p++) {
        for (size_t i = 0; i < n_installed; i++) {
            if (installed[i] && preferred[p] && strcmp(preferred[p], installed[i]) == 0) {
                if (!picked(out, n, (int)i)) {
                    out[n++] = (int)i;
                }
                break;
            }
        }
    }
    for (size_t i = 0; i < n_installed && n < max; i++) {
        if (!installed[i] || picked(out, n, (int)i)) {
            continue;
        }
        if (eligible && !eligible(installed[i])) {
            continue;
        }
        out[n++] = (int)i;
    }
    return n;
}

static const char *const WEEKDAYS[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

static const char *const MONTHS[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};

void home_format_date(int wday, int mday, int mon, bool two_lines,
                      char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    buf[0] = '\0';
    if (wday < 0 || wday > 6 || mon < 0 || mon > 11 || mday < 1 || mday > 31) {
        return;
    }
    snprintf(buf, len, "%s%c%d %s", WEEKDAYS[wday], two_lines ? '\n' : ' ',
             mday, MONTHS[mon]);
}

/* ------------------------------------------------------------------ */
/* What is new                                                          */
/* ------------------------------------------------------------------ */

/* Between two things on a row. Montserrat, LVGL's font, has the bullet
 * (U+2022) and not the middle dot; the call screen writes it the same way. */
#define DOT " \xE2\x80\xA2 "

/* "13:52", or "1:52 PM" on a 12-hour clock, as time_manager_format_time()
 * writes the time now. */
static void clock_face(int hour, int minute, bool format_24h, char *buf, size_t len)
{
    if (format_24h) {
        snprintf(buf, len, "%02d:%02d", hour, minute);
        return;
    }
    int h = hour % 12;
    snprintf(buf, len, "%d:%02d %s", h == 0 ? 12 : h, minute, hour < 12 ? "AM" : "PM");
}

static void messages_row(const home_now_in_t *in, home_now_row_t *row)
{
    uint32_t total = in->sms_unread + in->mesh_unread;
    row->kind = HOME_NOW_MESSAGES;
    if (total == 1) {
        snprintf(row->title, sizeof(row->title), "1 new message");
    } else {
        snprintf(row->title, sizeof(row->title), "%u new messages", (unsigned)total);
    }
    if (in->sms_unread > 0 && in->mesh_unread > 0) {
        snprintf(row->detail, sizeof(row->detail), "%u SMS" DOT "%u from the mesh",
                 (unsigned)in->sms_unread, (unsigned)in->mesh_unread);
    } else {
        snprintf(row->detail, sizeof(row->detail), "%s",
                 in->sms_unread > 0 ? "SMS" : "From the mesh");
    }
}

/* Who called last: the phonebook's name, else the number, else "Withheld";
 * "" when the log has no missed call to name. */
static const char *missed_caller(const home_now_in_t *in)
{
    if (in->missed_number == NULL) {
        return "";
    }
    if (in->missed_name && in->missed_name[0]) {
        return in->missed_name;
    }
    return in->missed_number[0] ? in->missed_number : "Withheld";
}

static void missed_call_row(const home_now_in_t *in, home_now_row_t *row)
{
    const char *who = missed_caller(in);
    row->kind = HOME_NOW_MISSED_CALL;
    if (in->missed_calls == 1) {
        snprintf(row->title, sizeof(row->title), "Missed call");
        char when[12] = "";
        if (in->missed_hour >= 0) {
            clock_face(in->missed_hour, in->missed_minute, in->format_24h,
                       when, sizeof(when));
        }
        if (who[0] && when[0]) {
            snprintf(row->detail, sizeof(row->detail), "%s" DOT "%s", who, when);
        } else {
            snprintf(row->detail, sizeof(row->detail), "%s", who);
        }
    } else {
        snprintf(row->title, sizeof(row->title), "%u missed calls",
                 (unsigned)in->missed_calls);
        if (who[0]) {
            snprintf(row->detail, sizeof(row->detail), "Last: %s", who);
        }
    }
    if (in->missed_number) {
        snprintf(row->call_back, sizeof(row->call_back), "%s", in->missed_number);
    }
}

static void tailnet_row(const home_now_in_t *in, home_now_row_t *row)
{
    row->kind = HOME_NOW_TAILNET;
    snprintf(row->title, sizeof(row->title), "Tailscale");
    snprintf(row->detail, sizeof(row->detail), "%s",
             in->tailnet == HOME_TAILNET_LOGIN ? "Sign in to join your tailnet"
                                               : "Waiting for an admin's approval");
}

static void update_row(const home_now_in_t *in, home_now_row_t *row)
{
    unsigned apps = (unsigned)in->app_updates;
    if (in->os_update != NULL && in->os_update[0] != '\0') {
        row->kind = HOME_NOW_SYSTEM_UPDATE;
        snprintf(row->title, sizeof(row->title), "System update");
        if (apps == 0) {
            snprintf(row->detail, sizeof(row->detail), "ThistleOS %s", in->os_update);
        } else {
            snprintf(row->detail, sizeof(row->detail), "ThistleOS %s and %u app%s",
                     in->os_update, apps, apps == 1 ? "" : "s");
        }
    } else {
        row->kind = HOME_NOW_APP_UPDATES;
        snprintf(row->title, sizeof(row->title), "App updates");
        snprintf(row->detail, sizeof(row->detail), "%u app%s", apps,
                 apps == 1 ? " has an update" : "s have updates");
    }
}

static void caught_up_row(home_now_row_t *row)
{
    row->kind = HOME_NOW_CAUGHT_UP;
    snprintf(row->title, sizeof(row->title), "All caught up");
    snprintf(row->detail, sizeof(row->detail), "No new messages or calls");
}

static void mesh_row(const home_now_in_t *in, home_now_row_t *row)
{
    row->kind = HOME_NOW_MESH;
    snprintf(row->title, sizeof(row->title), "Mesh on");
    if (in->mesh_contacts <= 0) {
        snprintf(row->detail, sizeof(row->detail), "No contacts yet");
    } else if (in->mesh_contacts == 1) {
        snprintf(row->detail, sizeof(row->detail), "1 contact");
    } else {
        snprintf(row->detail, sizeof(row->detail), "%d contacts", in->mesh_contacts);
    }
}

int home_now_rows(const home_now_in_t *in, home_now_row_t *out, int max)
{
    if (in == NULL || out == NULL || max <= 0) {
        return 0;
    }
    memset(out, 0, sizeof(*out) * (size_t)max);
    int n = 0;
    if (n < max && in->sms_unread + in->mesh_unread > 0) {
        messages_row(in, &out[n++]);
    }
    if (n < max && in->missed_calls > 0) {
        missed_call_row(in, &out[n++]);
    }
    if (n < max && in->tailnet != HOME_TAILNET_QUIET) {
        tailnet_row(in, &out[n++]);
    }
    if (n < max && ((in->os_update != NULL && in->os_update[0] != '\0') || in->app_updates > 0)) {
        update_row(in, &out[n++]);
    }
    if (n == 0) {
        caught_up_row(&out[n++]);
        if (n < max && in->mesh_running) {
            mesh_row(in, &out[n++]);
        }
    }
    return n;
}
