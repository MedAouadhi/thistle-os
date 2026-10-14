// SPDX-License-Identifier: BSD-3-Clause
#pragma once

/*
 * The call screen (call_ui.c): system UI over whatever app is open, under
 * the status bar, while a call rings here, is being placed, or is connected,
 * and for the moments after it ends while it says how. The kernel runs the
 * call (thistle/call.h); this follows rs_call_get_status() from an LVGL
 * timer and shows it.
 *
 * Back, Home or Esc tuck an outgoing or connected call away: the screen
 * closes and the status bar shows the call's time, which a tap reopens. A
 * call ringing here cannot be tucked away; Back and Esc silence it.
 *
 * LVGL window managers only. Everything here runs on the LVGL task with the
 * LVGL lock held, except call_ui_is_open(), which any task may ask.
 */

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "lvgl.h"

/* Start following calls. ui_manager_init() calls this once. */
esp_err_t call_ui_init(void);

/* Shapes `text` for display (joins Arabic letters, say): a malloc'd copy for
 * the caller to free(), or NULL to show `text` as it is. */
typedef char *(*call_ui_shape_fn)(const char *text);

/* The font, and the shaping, for names from the phonebook, which are not all
 * ASCII: the apps layer sets the Reader's font at startup. The font is
 * copied, with Montserrat 14 as its fallback if it has none; `shape` may be
 * NULL. Without this, names are in Montserrat 22, which has ASCII alone.
 * Numbers stay in Montserrat, which has their digits. Takes effect the next
 * time the call screen opens. LVGL task, or with the LVGL lock held. */
void call_ui_set_text_font(const lv_font_t *font, call_ui_shape_fn shape);

/* Whether the call screen is on screen. Safe from any task; a stale answer
 * only costs a gesture that is ignored. */
bool call_ui_is_open(void);

/* Whether a call is up but tucked away to the status bar. */
bool call_ui_is_tucked(void);

/* Back (or Esc) and Home, offered to the call screen before anything else.
 * Whether it took the key: false when it is not open, and for Home once the
 * call has ended, so Home goes on to the launcher. */
bool call_ui_back(void);
bool call_ui_home(void);

/* The status bar's call chip: the time of a call tucked away, or how many
 * calls were missed while none is up; false when there is nothing to show.
 * On e-paper the time is in whole minutes, so the text changes once a
 * minute. */
bool call_ui_chip_text(char *text, size_t size);

/* A tap on the chip: the call screen back, or the Phone app's missed calls. */
void call_ui_chip_tapped(void);

#ifdef SIMULATOR_BUILD
/* Show the call screen for a made-up call, whatever the kernel says, to look
 * at its layout on a device with no modem: "incoming", "calling", "active",
 * "keypad" or "ended". The simulator's --call-preview. */
void call_ui_preview(const char *what);
#endif
