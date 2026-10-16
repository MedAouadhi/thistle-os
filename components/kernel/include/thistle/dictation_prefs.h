// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* The language dictation is transcribed in (dictation_prefs.rs), kept in
 * system.json as thistle_os.dictation.language: an ISO 639-1 code, "ar"
 * until the user chooses. Cohere detects no language of its own, and told
 * the wrong one may answer translated. */

/* The language, NUL-terminated into `out`. False if `cap` cannot hold it. */
bool rs_dictation_get_language(char *out, size_t cap);

/* Dictate in `code` from now on, and save it. ESP_ERR_INVALID_ARG for
 * something that is not a language code; otherwise whether it was saved. */
esp_err_t rs_dictation_set_language(const char *code);
