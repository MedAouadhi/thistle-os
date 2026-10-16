// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* The system's volume and mute (audio_volume.rs), kept in system.json as
 * thistle_os.audio.volume and thistle_os.audio.muted and put back at boot.
 * The audio driver plays at 0 while muted and at the volume otherwise. */

/* Whether the board can play sound. */
bool rs_audio_available(void);

/* The volume, 0-100, muted or not. */
uint8_t rs_audio_get_volume(void);

/* Set the volume, and save it if `save`; while muted it is kept for when the
 * device is unmuted. The driver's error if it refused; otherwise whether the
 * volume was saved. */
esp_err_t rs_audio_set_volume(uint8_t percent, bool save);

bool rs_audio_is_muted(void);

/* Mute or unmute, and remember it. */
esp_err_t rs_audio_set_muted(bool muted);

/* Whether something is recording (audio_volume.rs). A recording calls
 * begin when it takes the microphone and end when it is done with it, and a
 * player pauses meanwhile: the microphone hears the speaker, and the codec
 * runs both directions at one rate, so a recording's format plays the music
 * at the wrong speed. Then the player sets its own format again. */
void rs_audio_capture_begin(void);
void rs_audio_capture_end(void);
bool rs_audio_capturing(void);
