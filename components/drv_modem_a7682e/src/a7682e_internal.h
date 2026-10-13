// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — A7682E driver internals shared by its source files
#pragma once

#include "esp_err.h"
#include "esp_modem_api.h"
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

esp_err_t a7682e_power_on_locked(void);
esp_err_t a7682e_power_off_locked(void);

/** Let a running module sleep listening for calls and SMS, with RI raised
 *  for them, or wake it to answer commands. ESP_ERR_INVALID_STATE to sleep a
 *  module that is off; ESP_ERR_NOT_SUPPORTED without a DTR line. */
esp_err_t a7682e_sleep_locked(bool enter);

/** Echo off, verbose errors, long operator names: lost when the module
 *  resets itself, so set again before each connection. */
void a7682e_configure_locked(void);

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
