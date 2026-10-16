/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — dictation's rules that need no hardware.
 *
 * Holding the microphone key records; letting go sends the recording to
 * Cohere's transcription API and inserts what it heard into the text field
 * that was focused. dictation.c does that with LVGL, the audio HAL and
 * esp_http_client; what is decided without them is here, for host tests.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What is recorded: what the Hermes bridge hands the same provider. */
#define DICTATION_SAMPLE_RATE      16000u
#define DICTATION_BYTES_PER_SECOND (DICTATION_SAMPLE_RATE * 2u)

/* Less audio than this is a tap, and is not sent. A recording stops itself
 * at the most, and the pill counts down from the warning. As on Hermes. */
#define DICTATION_MIN_MS  300u
#define DICTATION_WARN_MS 50000u
#define DICTATION_MAX_MS  60000u
#define DICTATION_MAX_PCM_BYTES (DICTATION_MAX_MS / 1000u * DICTATION_BYTES_PER_SECOND)

#define DICTATION_WAV_HEADER_BYTES 44u

/* The same provider, model and language as the Hermes agent's
 * cohere_api_stt.py. */
#define DICTATION_DEFAULT_URL      "https://api.cohere.com/v2/audio/transcriptions"
#define DICTATION_DEFAULT_MODEL    "cohere-transcribe-arabic-07-2026"
#define DICTATION_DEFAULT_LANGUAGE "ar"

#define DICTATION_URL_MAX      192
#define DICTATION_KEY_MAX      128
#define DICTATION_MODEL_MAX    64
#define DICTATION_LANGUAGE_MAX 16

typedef struct {
    char url[DICTATION_URL_MAX];
    char api_key[DICTATION_KEY_MAX];
    char model[DICTATION_MODEL_MAX];
    char language[DICTATION_LANGUAGE_MAX];
} dictation_config_t;

/* The defaults, with `api_key` (NULL for none) as the key. The language is
 * the user's, from the quick panel (thistle/dictation_prefs.h); the default
 * here is for a build that has none. */
void dictation_config_init(dictation_config_t *config, const char *api_key);

/* Each of "url", "api_key" and "model" that `json` has as a non-empty
 * string replaces the default; anything else is kept. False if `json` is not
 * a JSON object, with `config` as it was. */
bool dictation_config_apply(dictation_config_t *config, const char *json, size_t len);

/* The 44-byte header of a PCM WAV holding `pcm_bytes` of 16-bit mono at
 * `sample_rate`. */
void dictation_wav_header(uint8_t out[DICTATION_WAV_HEADER_BYTES], uint32_t sample_rate,
                          uint32_t pcm_bytes);

/* The multipart/form-data framing around the WAV: the model and language
 * fields and the file part's headers ahead of it, the closing boundary
 * after. The body sent is head, the WAV, tail. */
typedef struct {
    char head[384];
    size_t head_len;
    char tail[64];
    size_t tail_len;
    char content_type[96];  /* the request's Content-Type, boundary and all */
} dictation_form_t;

/* False if a value would not fit, or holds a character that would break the
 * framing (a quote, a line break). `boundary` is up to 40 characters that
 * cannot appear in the audio by chance: random, and long. */
bool dictation_form_build(dictation_form_t *form, const char *boundary, const char *model,
                          const char *language);

/* The string member `key` of the JSON object `json` (its top level only),
 * decoded into `out` as UTF-8, \u escapes and surrogate pairs included, and
 * cut between characters to fit. False, with "" in `out`, if there is none
 * or the text is not JSON. */
bool dictation_json_string(const char *json, size_t len, const char *key, char *out, size_t cap);

/* What a refusal from the service is, in words for a toast, given its status
 * and its `message` ("" if it gave none). */
void dictation_status_words(int status, const char *message, char *out, size_t cap);

/* What to insert into the field for `transcript`: trimmed, with a space
 * ahead of it when `before` (the byte before the cursor, 0 at the start of
 * the field) ends a word and the transcript does not open with punctuation
 * that closes one. In a one-line field, line breaks become spaces. Returns
 * the length written to `out`, 0 for a transcript of nothing but space. */
size_t dictation_join(char *out, size_t cap, const char *transcript, uint8_t before,
                      bool one_line);

/* The byte before character `pos` (as LVGL counts a cursor) of the UTF-8
 * text `txt`: dictation_join()'s `before`. 0 at the start. */
uint8_t dictation_byte_before(const char *txt, uint32_t pos);
