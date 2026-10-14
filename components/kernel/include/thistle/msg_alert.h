// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Alerts for messages as they arrive (msg_alert.rs). The kernel buzzes the
 * vibration motor for each SMS and MeshCore message it receives, Messenger
 * open or not, and keeps the latest for the UI to show as a pop-up. Neither
 * happens with alerts switched off (thistle_os.messenger.alerts, on unless
 * switched off), nor for a message in the conversation the Messenger has on
 * screen. */

#define MSG_ALERT_SMS   0
#define MSG_ALERT_MESH  1

typedef struct {
    uint32_t count;      /* messages since the last pop-up, this one included */
    uint8_t  transport;  /* MSG_ALERT_SMS or MSG_ALERT_MESH */
    char     from[33];   /* a contact's name, a number, or "Bob in Public" */
    char     text[73];   /* on one line, cut with "..." */
} msg_alert_t;

/* Take the pop-up waiting for the latest message; false when none is. */
bool rs_msg_alert_take(msg_alert_t *out);

/* The conversation the Messenger shows, as it addresses it: a phone number
 * for SMS, "" for the mesh's Public channel or a contact's address. A
 * transport of -1, or a NULL conv, is none. */
void rs_msg_alert_set_viewing(int transport, const char *conv);

bool rs_msg_alert_enabled(void);

/* Switch alerts on or off. Applies at once, even if saving the choice fails,
 * which the return reports. */
esp_err_t rs_msg_alert_set_enabled(bool on);
