/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — what the Podcasts app's pages share. Internal to the app.
 *
 * podcasts_ui.c has the pages and the stack they are on; pod_ui_player.c the
 * Now playing page. Everything here runs on the LVGL task.
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POD_UI_TITLE_H 30
#define POD_UI_PAD      8

/* The font text is set in: the Reader's DejaVu (its 1-bit cut on e-paper),
 * then the app's stars, then Montserrat for LVGL's symbols. */
const lv_font_t *pod_ui_font(void);

/* Whether the screen is e-paper, where every change is a refresh. */
bool pod_ui_epaper(void);

/* The app area's size. */
int32_t pod_ui_width(void);
int32_t pod_ui_height(void);

/* A page: a screen-filling container under the app's root with a title bar,
 * "< title", that goes back when tapped, and an optional button at its right
 * end (`*right_btn`, NULL not to have one). Returns the container; what goes
 * below the bar is placed from y = POD_UI_TITLE_H. */
lv_obj_t *pod_ui_page(const char *title, lv_obj_t **right_btn);

/* Sets a label's text, shaping any Arabic, only when it changed. */
void pod_ui_set_text(lv_obj_t *label, const char *text);

/* A button with a label: the label's text is set by pod_ui_set_text. */
lv_obj_t *pod_ui_button(lv_obj_t *parent, const char *text, const lv_font_t *font,
                        lv_event_cb_t clicked, void *user_data);
lv_obj_t *pod_ui_button_label(lv_obj_t *button);

/* Puts `obj` in the keypad's group and focuses it. */
void pod_ui_focus(lv_obj_t *obj);

/* Whether the event that is being handled came from the touch panel. */
bool pod_ui_from_touch(void);

/* Goes up a page. False on the first page. */
bool pod_ui_back(void);

/* Opens the Now playing page. */
void pod_ui_open_player(void);

/* The Now playing page (pod_ui_player.c). build makes it under `page`,
 * whose title bar's star is `star`; tick runs every 250 ms while it is
 * shown; key handles its keys; release forgets its widgets when the page
 * goes. */
void pod_ui_player_build(lv_obj_t *page, lv_obj_t *star);
void pod_ui_player_tick(void);
bool pod_ui_player_key(uint32_t key);
void pod_ui_player_release(void);

/* Keys every page takes: p plays or pauses, and so on. True when used. */
bool pod_ui_common_key(uint32_t key);
