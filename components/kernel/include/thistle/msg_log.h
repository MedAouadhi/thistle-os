// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* The message history (msg_log.rs): every SMS and MeshCore message received
 * or sent, one JSON object per line, in a file per conversation under
 * rs_msg_log_dir():
 *
 *   sms/+4917612345678.jsonl
 *   mesh/public.jsonl
 *   mesh/1a2b3c4d5e6f.jsonl     a contact, by the start of its key
 *
 * Kept unless switched off, as thistle_os.messenger.save_history, and read
 * back by the Messenger after a restart. */

bool rs_msg_log_enabled(void);

/* Switch recording on or off. Applies at once, even if saving the choice
 * fails, which the return reports. */
esp_err_t rs_msg_log_set_enabled(bool on);

/* Whether the history can be written: false without an SD card. */
bool rs_msg_log_writable(void);

const char *rs_msg_log_dir(void);

/* Which history a message read back is from. */
#define MSG_LOG_SMS   0
#define MSG_LOG_MESH  1

/* A message read back. `conv` is the conversation as the Messenger
 * addresses it: a phone number or a sender's name, a mesh contact's key
 * prefix, or "" for the mesh's Public channel. `peer` is who it came from,
 * or went to if `sent`; `time` its time of day, "14:05" or "2:05 PM", or
 * "--:--" if not known. The strings last for the call only. */
typedef void (*msg_log_saved_cb_t)(void *ctx, int transport, const char *conv,
                                   bool sent, const char *peer, const char *text,
                                   const char *time);

/* Hand `cb` the history from before this boot, to show again after a
 * restart: the `max_convs` conversations with the latest messages, the
 * latest last, each with its last `max_msgs` messages, oldest first. What
 * this boot saved is left out, as it reached the app from the kernel's
 * inboxes or was sent by it. Reads the card on the caller's task. Returns
 * how many messages it handed over. */
int rs_msg_log_read_back(uint32_t max_convs, uint32_t max_msgs,
                         msg_log_saved_cb_t cb, void *ctx);
