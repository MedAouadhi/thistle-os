/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Hermes app's screens, as they share them.
 *
 * hermes_ui.c has the page that says what the connection is waiting for,
 * the header, the transcript and the compose bar; hermes_panels.c the
 * panels that open over them: the drawer, the Command panel, Scheduled
 * Runs and the bridge's address. Both run on the LVGL task only.
 */
#pragma once

#include "hermes/hermes_bridge.h"
#include "hermes/hermes_chat.h"

#include "esp_err.h"
#include "lvgl.h"

#include <stdbool.h>

typedef enum {
    HERMES_PANEL_NONE,
    HERMES_PANEL_DRAWER,     /* sessions, New conversation, the rest */
    HERMES_PANEL_COMMANDS,
    HERMES_PANEL_JOBS,       /* Scheduled Runs: the jobs */
    HERMES_PANEL_RUN,        /* one job's runs, one at a time */
    HERMES_PANEL_SETUP,      /* the bridge's address */
} hermes_panel_t;

/* hermes_app.c's */
esp_err_t hermes_ui_create(lv_obj_t *parent);
void hermes_ui_destroy(void);

/* hermes_ui.c's, for the panels */
const hermes_chat_t *hermes_ui_chat(void);
/* Takes a fresh copy of the chat state now, rather than at the next tick. */
void hermes_ui_refresh_chat(void);
const hermes_link_status_t *hermes_ui_link(void);
const char *hermes_ui_url(void);
/* The address chosen on the setup page, used from now on. */
void hermes_ui_set_url(const char *url);
/* Writes `text` into the draft at its cursor. */
void hermes_ui_insert(const char *text);
/* A panel closed: the keyboard goes back to the draft. */
void hermes_ui_panel_closed(void);
void hermes_ui_add_focus(lv_obj_t *obj);

/* hermes_panels.c's */
void hermes_panels_open(lv_obj_t *root, int32_t top, hermes_panel_t panel);
void hermes_panels_close(void);
hermes_panel_t hermes_panels_current(void);
/* Back: the panel under this one, or none. False if none was open. */
bool hermes_panels_back(void);
/* The chat state changed: what is open redraws if its part did. */
void hermes_panels_refresh(void);

/* "now", "5 min", "3 h", "2 d": how long ago, as the bridge counted it. */
void hermes_ui_age(uint32_t seconds, char *out, size_t cap);
