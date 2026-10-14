/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — AI Assistant setup screen
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "hal/sdcard_path.h"

#define ASSISTANT_CONFIG_PATH THISTLE_SDCARD "/config/assistant.json"

typedef void (*assistant_setup_done_cb_t)(bool saved);

/* Open the setup form over parent, filled from ASSISTANT_CONFIG_PATH. done
 * runs once, when the form is saved or cancelled, just before the form is
 * deleted; it should move keyboard focus elsewhere. */
void assistant_setup_open(lv_obj_t *parent, assistant_setup_done_cb_t done);

/* Delete the form without saving or calling done, if it is open. */
void assistant_setup_close(void);

/* Leave the form without saving, as tapping its title bar does: done runs. */
void assistant_setup_cancel(void);

bool assistant_setup_is_open(void);
