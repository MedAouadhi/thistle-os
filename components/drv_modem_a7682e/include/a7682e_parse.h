// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — reading the A7682E's answers about its SIM and its network
//
// Pure functions over the text a command returned, so they can be tested on
// the host. Command forms are those of the A76XX Series AT Command Manual
// V1.09: AT+CPIN? (6.2.2), AT+CEREG? (5.2.2), AT+CGREG? (5.2.1),
// AT+CREG? (4.2.1), AT+COPS? (4.2.2), AT+CGDCONT? (5.2.5), AT+CCLK?
// (3.2.10), and for calls ATD and ATA (2.2.1, 2.2.2) and AT+CLCC (7.2.6).
#pragma once

#include "hal/network.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Whether an answer has ended: 1 once it holds an "OK" line, -1 once it holds
 * an "ERROR", "+CME ERROR: ..." or "+CMS ERROR: ..." line, 0 while it is still
 * arriving. Only whole lines count, so an error is not taken for final before
 * the text saying what it is has arrived.
 */
int a7682e_final_result(const char *buf);

/**
 * As a7682e_final_result(), for ATD and ATA, which can also end on the result
 * code of a call that did not happen: "NO CARRIER", "BUSY", "NO ANSWER" or
 * "NO DIALTONE" (manual 2.2.1, 2.2.2 and 28.1) reads as -1. ATA with nothing
 * ringing answers NO CARRIER. Not for other commands: the module also sends
 * NO CARRIER unasked when a call ends, which must not end their exchange.
 */
int a7682e_call_final_result(const char *buf);

/**
 * What an AT+CPIN? exchange says about the SIM, given everything the modem
 * sent back. With AT+CMEE=2 a missing SIM reads "+CME ERROR: SIM not
 * inserted"; with AT+CMEE=1 it reads "+CME ERROR: 10". HAL_SIM_UNKNOWN for a
 * SIM still being read ("SIM busy", error 14) or an answer that names
 * nothing, so the caller asks again.
 */
hal_sim_state_t a7682e_parse_sim(const char *resp);

/**
 * The registration status in a "+CEREG:", "+CGREG:" or "+CREG:" line, read
 * either as the read command's "<n>,<stat>[,...]" or the bare "<stat>" of a
 * URC. "Registered for SMS only" (6, 7) and emergency-only attach (11) carry
 * no data, so they count as not registered.
 */
hal_cell_reg_t a7682e_parse_reg(const char *resp);

/** The better of two registration states, for an LTE (CEREG) and a 2G
 *  (CGREG) answer that disagree: registered beats searching beats denied
 *  beats not registered beats unknown. */
hal_cell_reg_t a7682e_reg_best(hal_cell_reg_t a, hal_cell_reg_t b);

/** True for a state in which a data session can be opened. */
static inline bool a7682e_reg_is_registered(hal_cell_reg_t reg)
{
    return reg == HAL_CELL_REG_HOME || reg == HAL_CELL_REG_ROAMING;
}

/**
 * The operator name quoted in "+COPS: <mode>,<format>,\"<oper>\"[,<AcT>]",
 * cut to fit `out`. False, with `out` emptied, when the line names no
 * operator ("+COPS: 0" while unregistered).
 */
bool a7682e_parse_operator(const char *resp, char *out, size_t out_size);

/**
 * The APN of context `cid` in an AT+CGDCONT? answer, one
 * "+CGDCONT: <cid>,\"<PDP_type>\",\"<APN>\"[,...]" line per context, cut to
 * fit `out`. For an active default bearer the module reports the APN the
 * network assigned. False, with `out` emptied, when no line names `cid` or
 * its APN is empty.
 */
bool a7682e_parse_context_apn(const char *resp, int cid, char *out, size_t out_size);

/**
 * The time in an AT+CCLK? answer, "+CCLK: \"yy/MM/dd,hh:mm:ss±zz\"" (3.2.10):
 * the modem's clock in local time, and zz the offset from UTC in quarters of
 * an hour, summer time included, which the network sets with AT+CTZU=1.
 * `utc` is seconds since 1970 and `offset_min` local time minus UTC. Years
 * 70 to 99 are read as 19yy and 00 to 69 as 20yy, so a clock counting from
 * 1970 or 1980 reads as that. False for an answer without the offset, whose
 * local time does not say what time it is.
 */
bool a7682e_parse_cclk(const char *resp, int64_t *utc, int *offset_min);

/**
 * The voice calls in an AT+CLCC answer (7.2.6), one line each:
 * "+CLCC: <id>,<dir>,<stat>,<mode>,<mpty>[,\"<number>\",<type>[,\"<alpha>\"]
 * [,<priority>][,<CLI validity>]]", with or without a space after each comma,
 * and the last two fields only on a 2G call. A call of another bearer (<mode>
 * 1 data, 2 fax, 9 unknown) is passed over. A withheld number is "", and a
 * longer one than hal_call_t holds is cut. Lines that are not +CLCC (a RING
 * or +CLIP the module sent unasked) are passed over too.
 *
 * The first `max` calls go into calls[] (which may be NULL when max is 0).
 * Returns how many voice calls the answer lists, which may be more than max:
 * 0 when there are none, -1 when a +CLCC line cannot be read, which says
 * nothing about whether there are calls.
 */
int a7682e_parse_clcc(const char *resp, hal_call_t *calls, int max);
