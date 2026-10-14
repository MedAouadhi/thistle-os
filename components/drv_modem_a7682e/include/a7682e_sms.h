// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — reading the SMS the A7682E holds
//
// Pure functions over the text a command returned, so they can be tested on
// the host. Command forms are those of the A76XX Series AT Command Manual
// V1.09: AT+CPMS? (9.2.3), AT+CMGR (9.2.12) and AT+CMGD=? (9.2.16), read in
// text mode (AT+CMGF=1) with the header's every field shown (AT+CSDH=1).
// What the fields mean is 3GPP TS 23.040 (first octet, user data header) and
// TS 23.038 (data coding scheme, the GSM 7-bit default alphabet).
#pragma once

#include "esp_err.h"
#include "hal/network.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * How many messages the storage holds, from an AT+CPMS? answer:
 * "+CPMS: \"ME\",<used>,<total>,\"ME\",<used>,<total>,..." gives the first
 * <used>. -1 when the answer has no +CPMS line.
 */
int a7682e_parse_storage_used(const char *resp);

/**
 * The storage indices an AT+CMGD=? answer lists: the first parenthesised
 * list of "+CMGD: (1,2,5-7),(0-4)", ranges expanded, at most `max` of them.
 * Returns how many were written, 0 for "()", or -1 when the answer has no
 * +CMGD line.
 */
int a7682e_parse_index_list(const char *resp, int *out, int max);

/**
 * The message in an AT+CMGR answer:
 *
 *   +CMGR: <stat>,<oa>,[<alpha>],<scts>[,<tooa>,<fo>,<pid>,<dcs>,<sca>,<tosca>,<length>]
 *   <data>
 *   OK
 *
 * `ucs2` says whether AT+CSCS="UCS2" was in force, under which <oa> and the
 * text of a 7-bit message arrive as UCS2 in hex. Text is decoded by <dcs>
 * and <fo>: GSM 7-bit, 8-bit or UCS2, with a user data header (a part of a
 * long message) taken off and read into part, parts and concat_ref. Every
 * string is cut to fit at a character boundary and ends in NUL.
 *
 * @return ESP_OK for a received message (SMS-DELIVER),
 *         ESP_ERR_NOT_SUPPORTED for anything else the storage can hold (a
 *         status report, a message stored to send, a broadcast), and
 *         ESP_ERR_INVALID_RESPONSE for an answer that cannot be read.
 */
esp_err_t a7682e_parse_cmgr(const char *resp, bool ucs2, hal_sms_t *out);
