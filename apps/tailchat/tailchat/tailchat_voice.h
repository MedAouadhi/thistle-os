// SPDX-License-Identifier: BSD-3-Clause
#pragma once

/* Voice messages' audio: recording one as Ogg/Opus at 16 kHz mono, and
 * playing one. Each runs on a FreeRTOS task of its own with a 20 KB
 * internal stack (ticket 01's measure of what libopus needs beside its
 * PSRAM pseudostack), made for it and gone after; the audio is in PSRAM.
 * Both take the microphone's place as dictation does
 * (rs_audio_capture_begin), so a player such as Podcasts pauses meanwhile
 * and plays on after, and both take Opus (thistle_opus_take). One at a
 * time; the UI polls from its lv_timer. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TAILCHAT_VOICE_MAX_MS 120000  /* a recording stops at 2 minutes */
#define TAILCHAT_VOICE_MIN_MS 300     /* under this, it was a tap */

typedef enum {
    TAILCHAT_VOICE_IDLE,
    TAILCHAT_VOICE_RECORDING,
    TAILCHAT_VOICE_PLAYING,
    /* A recording has ended: tailchat_voice_take() has it. */
    TAILCHAT_VOICE_RECORDED,
} tailchat_voice_state_t;

/* Start recording; NULL, or why not, for a toast. */
const char *tailchat_voice_record(void);

/* Start playing the Ogg/Opus file at `path`, as message `id`; NULL, or why
 * not. */
const char *tailchat_voice_play(const char *path, const char *id);

/* Stop recording (the recording is then taken) or playing. */
void tailchat_voice_stop(void);

/* What is happening; *ms is how much has been recorded or played. *id is
 * the message playing ("" otherwise). */
tailchat_voice_state_t tailchat_voice_poll(uint32_t *ms, const char **id);

/* The recording ended: its Ogg/Opus file and length, or NULL with why (a
 * tap, a failure) in *why. The file is the caller's until
 * tailchat_voice_release(); then the module is idle again. */
const uint8_t *tailchat_voice_take(size_t *len, uint32_t *ms, const char **why);
void tailchat_voice_release(void);

#ifdef __cplusplus
}
#endif
