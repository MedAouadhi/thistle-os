// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* The message history (msg_log.rs): every SMS and MeshCore message received
 * or sent, one JSON object per line, in a file per conversation under
 * rs_msg_log_dir():
 *
 *   sms/+4917612345678.jsonl
 *   mesh/public.jsonl
 *   mesh/1a2b3c4d5e6f.jsonl     a contact, by the start of its key
 *
 * Kept unless switched off, as thistle_os.messenger.save_history. */

bool rs_msg_log_enabled(void);

/* Switch recording on or off. Applies at once, even if saving the choice
 * fails, which the return reports. */
esp_err_t rs_msg_log_set_enabled(bool on);

/* Whether the history can be written: false without an SD card. */
bool rs_msg_log_writable(void);

const char *rs_msg_log_dir(void);
