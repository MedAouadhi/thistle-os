// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* SMS, as the kernel keeps them (sms_manager.rs). The network coordinator
 * fetches each SMS from the modem as it arrives, whether or not an app is
 * open, records it in the message history (thistle/msg_log.h), and queues it
 * here. A long message that came in parts is queued once, whole. */

#define SMS_SENDER_MAX  33
#define SMS_TEXT_MAX    1024

typedef struct {
    char     sender[SMS_SENDER_MAX];  /* "+4917612345678", or a name such as "DHL" */
    char     text[SMS_TEXT_MAX];      /* UTF-8, cut at a character to fit */
    uint32_t sent;                    /* by the service centre's clock, seconds
                                         since 1970; 0 if unknown */
} sms_message_t;

/* Whether an SMS can be sent: the modem is on with a SIM ready. */
bool rs_sms_ready(void);

/* Send `text`, up to 160 plain ASCII characters, to `number` ('+' and
 * digits, or digits), and record it in the history. Blocks for the exchange
 * with the modem: seconds, with a data session to interrupt. */
esp_err_t rs_sms_send(const char *number, const char *text);

/* Take the oldest received SMS; false when there is none. */
bool rs_sms_inbox_pop(sms_message_t *out);
uint32_t rs_sms_inbox_count(void);

/* SMS received that the user has not read: those still waiting, and those
 * the app that took them reported unread. The app that takes them reports how
 * many it has still to show, again whenever that changes. */
uint32_t rs_sms_unread_count(void);
void rs_sms_set_held_unread(uint32_t count);
