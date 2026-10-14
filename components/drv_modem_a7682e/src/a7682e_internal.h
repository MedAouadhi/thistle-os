// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — A7682E driver internals shared by its source files
#pragma once

#include "esp_err.h"
#include "esp_modem_api.h"
#include "hal/network.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The answer buffer every esp_modem_at() / esp_modem_at_raw() call needs:
 * both copy up to this many bytes, whatever buffer they are given. */
#define A7682E_AT_BUF CONFIG_ESP_MODEM_C_API_STR_MAX

/* Room for a whole multi-line answer: an SMS header and its 160-character
 * body, or a short message list. */
#define A7682E_EXCHANGE_BUF 512

/* Everything below except a7682e_ppp_up() and a7682e_ppp_ip() must be called
 * between a7682e_lifecycle_begin_operation() and _finish_operation(). */

/** The DCE, or NULL while the module is off. */
esp_modem_dce_t *a7682e_dce_locked(void);

/**
 * Send `cmd` (with its trailing "\r") and copy everything the module answers,
 * every line of it, into `resp`, up to the final result.
 *
 * esp_modem_at() keeps only an answer's last line, which loses the header of
 * AT+CMGR and the "+CME ERROR: SIM not inserted" of AT+CPIN?; matching
 * esp_modem_at_raw() on "ERROR" ends on a half-received error line. This
 * waits for a whole final line (a7682e_final_result()).
 *
 * @return ESP_OK on OK, ESP_FAIL on an error line, ESP_ERR_TIMEOUT without
 *         either; `resp` holds what arrived in every case.
 */
esp_err_t a7682e_exchange_locked(const char *cmd, char *resp, size_t resp_size,
                                 uint32_t timeout_ms);

/**
 * a7682e_exchange_locked() for the call commands. What the module sent
 * unasked since the last exchange waits on the UART to be read as the next
 * answer: RING and +CLIP while a call rings, VOICE CALL: END and NO CARRIER
 * when one ends. It is thrown away first, so none of it is taken for this
 * command's answer. With `dial` (ATD, ATA), the result codes of a call that
 * failed end the exchange too (a7682e_call_final_result()).
 */
esp_err_t a7682e_call_exchange_locked(const char *cmd, char *resp, size_t resp_size,
                                      uint32_t timeout_ms, bool dial);

/** Whether the module sleeps (AT+CSCLK=1 with DTR high), answering nothing
 *  until a7682e_sleep_locked(false). */
bool a7682e_asleep_locked(void);

/** The RI pin, or -1 without one. Fixed at init, so read without the driver. */
int a7682e_ri_pin(void);

/** When RI last fell, by esp_timer, as its interrupt saw it: false if it has
 *  not been seen to. The interrupt is off while the power manager has the
 *  pin as a wake source, so a fall may go unrecorded. Without the driver. */
bool a7682e_ri_fell_at(int64_t *us);

esp_err_t a7682e_power_on_locked(void);
esp_err_t a7682e_power_off_locked(void);

/** Let a running module sleep listening for calls and SMS, with RI raised
 *  for them, or wake it to answer commands. ESP_ERR_INVALID_STATE to sleep a
 *  module that is off; ESP_ERR_NOT_SUPPORTED without a DTR line. */
esp_err_t a7682e_sleep_locked(bool enter);

/** Echo off, verbose errors, long operator names: lost when the module
 *  resets itself, so set again before each connection. */
void a7682e_configure_locked(void);

/** Read the module's clock (AT+CCLK?) for drv_a7682e_get_network_time().
 *  In command mode only. */
void a7682e_clock_read_locked(void);

/** The same, taking the driver itself, for a module answering commands;
 *  a data session is left alone. */
void a7682e_clock_read(void);

/** Leave a data session for command mode, if the DCE is in one. Returns
 *  whether it was, for a7682e_restore_data_locked(). */
bool a7682e_enter_command_locked(void);
void a7682e_restore_data_locked(bool was_data);

/** Whether a7682e_restore_data_locked() put the DCE back into data mode
 *  within the last `ms`: the session an SMS exchange interrupted is being
 *  redialled, rather than dead. */
bool a7682e_resumed_within_locked(uint32_t ms);

/** True while the PPP link holds an address. */
bool a7682e_ppp_up(void);

/** That address, dotted; "" while the link has none. */
const char *a7682e_ppp_ip(void);

/* -------------------------------------------------------------------------
 * Voice calls (a7682e_call.c): the call functions of hal_net_driver_t, which
 * take the driver themselves.
 * ---------------------------------------------------------------------- */

esp_err_t a7682e_call_dial(const char *number);
esp_err_t a7682e_call_answer(void);
esp_err_t a7682e_call_hangup(void);
esp_err_t a7682e_call_list(hal_call_t *calls, int max, int *count);
bool      a7682e_call_ringing(void);
esp_err_t a7682e_call_dtmf(char digit);
esp_err_t a7682e_call_set_mute(bool mute);
esp_err_t a7682e_call_set_volume(uint8_t percent);
esp_err_t a7682e_call_set_speaker(bool speaker);

/** Whether a call holds the line: from the first call command until
 *  AT+CLCC lists no call. Meanwhile no data session is dialled and the
 *  module is not put to sleep. Read without the driver. */
bool a7682e_call_holds_line(void);

/** Forget every call and the data session one interrupted: the module is
 *  powering off. */
void a7682e_call_forget_locked(void);
