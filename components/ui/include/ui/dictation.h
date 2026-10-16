/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — dictation: hold the microphone key, speak, let go.
 *
 * Held while a text field has the keyboard's focus, HAL_KEY_MIC records from
 * the microphone. Let go, the recording goes to Cohere's transcription API,
 * and what it heard is inserted at that field's cursor. A pill at the top of
 * the screen says Listening, then Transcribing; a toast says why when
 * nothing comes of it. .scratch/dictation/spec.md has the rules.
 *
 * System UI for the LVGL window managers, like the quick panel: the UI
 * manager hands it the key. The recording and the upload run on a task of
 * their own, one per dictation; everything here runs where LVGL does.
 */
#pragma once

#include <stdbool.h>

/* Set up the timer that runs dictation. Call once, from ui_manager_init(). */
void dictation_init(void);

/* The microphone key went down or came up. Called from the input callback,
 * which runs on the LVGL task outside the LVGL lock; acted on from a timer. */
void dictation_key(bool down);

/* Whether a dictation is recording or being transcribed. */
bool dictation_active(void);
