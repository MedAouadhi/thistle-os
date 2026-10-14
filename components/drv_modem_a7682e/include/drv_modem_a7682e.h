// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — Simcom A7682E 4G LTE modem driver header
#pragma once

#include "esp_err.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "hal/network.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A7682E hardware and UART configuration.
 *
 * Pass this to drv_a7682e_init().  baud_rate defaults to 115200 if set to 0.
 *
 * PWRKEY is reached either through a SoC GPIO (pin_pwrkey) or through an IO
 * expander pin (pwrkey_expander >= 0), and on some boards through a
 * transistor that inverts it: pwrkey_press_level is the level that presses
 * the key. The module's own PWRKEY is active-low, so a GPIO wired straight to
 * it presses at 0; the T-Deck Max's XL9555 P10 drives an NPN that pulls
 * PWRKEY low, so it presses at 1.
 */
typedef struct {
    uart_port_t uart_num;
    gpio_num_t  pin_tx;      ///< SoC TX, wired to the module's RXD
    gpio_num_t  pin_rx;      ///< SoC RX, wired to the module's TXD
    gpio_num_t  pin_pwrkey;  ///< Power key on a SoC GPIO, or -1
    gpio_num_t  pin_reset;   ///< Hardware reset — active-low, or -1
    gpio_num_t  pin_dtr;     ///< DTR, held low to keep the module awake, or -1
    gpio_num_t  pin_ri;      ///< RI, pulsed low when an SMS arrives, or -1
    uint32_t    baud_rate;   ///< The module's power-up rate; 0 → its default, 115200
    /** Rate both ends switch to (AT+IPR) once the module answers, or 0 to stay
     *  at baud_rate. One the module lists: up to 3686400. With no RTS/CTS, a
     *  rate the SoC cannot keep up with loses bytes. */
    uint32_t    fast_baud_rate;
    int         pwrkey_expander;      ///< Expander carrying PWRKEY, or -1
    int         pwrkey_expander_pin;
    int         pwrkey_press_level;   ///< Level that presses PWRKEY
    int         power_expander;       ///< Expander switching the supply, or -1
    int         power_expander_pin;   ///< Driven high to power the module
} a7682e_config_t;

/**
 * @brief Network registration status values returned by
 *        drv_a7682e_get_network_reg().
 */
typedef enum {
    A7682E_NET_NOT_REGISTERED   = 0, ///< Not registered, not searching
    A7682E_NET_REGISTERED_HOME  = 1, ///< Registered, home network
    A7682E_NET_SEARCHING        = 2, ///< Not registered, searching
    A7682E_NET_DENIED           = 3, ///< Registration denied
    A7682E_NET_UNKNOWN          = 4, ///< Unknown
    A7682E_NET_REGISTERED_ROAM  = 5, ///< Registered, roaming
} a7682e_net_reg_t;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/**
 * @brief Initialise the modem driver.
 *
 * Installs the UART driver, configures PWRKEY and RESET GPIOs as outputs
 * (both initially high / de-asserted).  Does NOT power the modem on.
 *
 * @param config  Hardware configuration.  Must not be NULL.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG, or an ESP-IDF driver error.
 */
esp_err_t drv_a7682e_init(const a7682e_config_t *config);

/**
 * @brief De-initialise the driver.
 *
 * Powers off the modem if it is on, then uninstalls the UART driver and
 * releases GPIO resources.
 */
void drv_a7682e_deinit(void);

/* -------------------------------------------------------------------------
 * Power control
 * ---------------------------------------------------------------------- */

/**
 * @brief Power the modem on or off.
 *
 * Power-on switches the supply on (where the board can), then waits for the
 * module to answer AT. A module the SoC left running across its own reset
 * answers at once, or once a stale data session is escaped; only a module
 * that does neither gets a 100 ms PWRKEY press, and then has 15 s to answer.
 * Echo is turned off and errors made verbose.
 *
 * Power-off shuts a module that answers down with AT+CPOF, waits for it to
 * finish, and only then switches the supply off: cutting the supply of a
 * running A7682E can corrupt its flash.
 *
 * @param on  true → power on, false → power off.
 * @return ESP_OK, ESP_ERR_TIMEOUT, or an ESP-IDF error.
 */
esp_err_t drv_a7682e_power(bool on);

/* -------------------------------------------------------------------------
 * AT command interface
 * ---------------------------------------------------------------------- */

/**
 * @brief Send a raw AT command and capture the response.
 *
 * Flushes the RX FIFO, writes "cmd\r\n" to the UART, then reads bytes until
 * "OK\r\n" or "ERROR\r\n" is found in the accumulator, or until timeout_ms
 * elapses.  The full response (including the echo-off prefix, if any, the
 * result code, and everything in between) is written to buf as a
 * null-terminated string.
 *
 * @param cmd         AT command string (without trailing CR/LF).
 * @param buf         Caller-supplied buffer for the response.  May be NULL.
 * @param buf_len     Size of buf in bytes.
 * @param timeout_ms  Read timeout in milliseconds; 0 → use default (5000 ms).
 * @return ESP_OK if "OK" was received, ESP_FAIL if "ERROR" was received,
 *         ESP_ERR_TIMEOUT on timeout, ESP_ERR_INVALID_STATE if not init'd.
 */
esp_err_t drv_a7682e_send_at(const char *cmd, char *buf, size_t buf_len,
                              uint32_t timeout_ms);

/**
 * @brief Return true if the modem responds to "AT" within 1 second.
 */
bool drv_a7682e_is_ready(void);

/* -------------------------------------------------------------------------
 * Network helpers
 * ---------------------------------------------------------------------- */

/**
 * @brief Query received signal strength.
 *
 * Sends AT+CSQ and parses the "+CSQ: rssi,ber" response.
 *
 * @return Signal strength in dBm (range −113 to −51), or −999 if the
 *         module reports 99 (unknown) or does not respond.
 */
int drv_a7682e_get_signal_rssi(void);

/**
 * @brief Query network registration status.
 *
 * Sends AT+CREG? and parses the "+CREG: n,stat" response.
 *
 * @return One of the a7682e_net_reg_t values, or A7682E_NET_UNKNOWN on error.
 */
a7682e_net_reg_t drv_a7682e_get_network_reg(void);

/* -------------------------------------------------------------------------
 * Data / connectivity stubs (future use)
 * ---------------------------------------------------------------------- */

/** @brief Open a TCP connection to host:port. */
esp_err_t drv_a7682e_connect_tcp(const char *host, uint16_t port);

/** @brief Send raw bytes over an open socket. */
esp_err_t drv_a7682e_send_data(const uint8_t *data, size_t len);

/**
 * @brief Perform an HTTP GET request and store the body in buf.
 *
 * With PPP active, prefer using esp_http_client directly — it routes through
 * the PPP netif automatically, just like WiFi.
 */
esp_err_t drv_a7682e_http_get(const char *url, char *buf, size_t buf_len);

/* -------------------------------------------------------------------------
 * PPP data connection (routes ESP-IDF TCP/IP stack through 4G)
 * ---------------------------------------------------------------------- */

/**
 * @brief Switch the modem to PPP data mode and obtain an IP address.
 *
 * After this returns ESP_OK, the entire ESP-IDF networking stack (sockets,
 * esp_http_client, MQTT, …) is routed over the 4G connection — no
 * application-level changes are needed beyond calling this function.
 *
 * @return ESP_OK on successful PPP link-up, ESP_ERR_TIMEOUT if no IP address
 *         was obtained within 30 s, ESP_ERR_INVALID_STATE if modem is off.
 */
esp_err_t drv_a7682e_start_ppp(void);

/**
 * @brief Return the modem to AT command mode and tear down the PPP link.
 *
 * @return ESP_OK, or an ESP-IDF error if the mode switch failed.
 */
esp_err_t drv_a7682e_stop_ppp(void);

/**
 * @brief Return true if the PPP link is up and an IP address has been
 *        assigned.
 */
bool drv_a7682e_ppp_connected(void);

/* -------------------------------------------------------------------------
 * SMS
 *
 * Received messages stay in the module's storage until fetched. The
 * cellular transport (drv_a7682e_net_driver) offers these as its sms_send,
 * sms_due and sms_fetch.
 * ---------------------------------------------------------------------- */

/**
 * @brief Send an SMS in text mode.
 *
 * Interrupts a data session for the length of the exchange.
 *
 * @param phone  Destination number: optional leading '+', then digits only.
 * @param msg    Message text (max 160 chars; controls and quotes rejected).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if either field is unsafe,
 *         ESP_ERR_INVALID_STATE if modem is off.
 */
esp_err_t drv_a7682e_send_sms(const char *phone, const char *msg);

/**
 * @brief Whether the module may hold SMS not yet fetched.
 *
 * True once after each power-on, outside a data session; whenever RI has
 * fallen since the last fetch (in a data session, no sooner than 30 s after
 * it); and periodically, often without an RI line and rarely with one. A
 * data session without an RI line is looked into every ten minutes.
 */
bool drv_a7682e_sms_due(void);

/**
 * @brief Hand every SMS in the module's storage to cb, and delete each one
 *        cb returns true for.
 *
 * Runs with the driver held, leaving a data session for command mode and
 * resuming it after: cb must not call the driver.
 *
 * @param kept  Set to how many cb kept. May be NULL.
 * @return ESP_OK, ESP_ERR_INVALID_STATE with the module off or asleep, or
 *         the error of the exchange that failed.
 */
esp_err_t drv_a7682e_sms_fetch(hal_sms_rx_cb_t cb, void *user_data, int *kept);

/**
 * @brief The module's clock as last read, which the network sets (NITZ,
 *        with AT+CTZU=1).
 *
 * Read after each registration and at each SMS fetch, never by interrupting
 * a data session for it. A module whose clock nothing has set reports the
 * epoch it counts from; the reader judges. Kept across a power-down, aging
 * by esp_timer, which keeps counting through light sleep.
 *
 * @return ESP_OK, or ESP_ERR_NOT_FOUND until the clock has been read.
 */
esp_err_t drv_a7682e_get_network_time(hal_net_time_t *out);

/* -------------------------------------------------------------------------
 * Network transport
 * ---------------------------------------------------------------------- */

/**
 * @brief The modem as a cellular network transport, for net_manager_register().
 *
 * connect(apn, NULL, timeout_ms) powers the module, waits for a ready SIM and
 * a registration, and opens a PPP data session on the APN — the one the
 * network assigns when apn is NULL or empty. It fails fast on a SIM that is
 * missing or locked. disconnect() powers the module off. get_cell_info()
 * reports what the last attempt saw of the SIM and the network.
 */
const hal_net_driver_t *drv_a7682e_net_driver(void);

#ifdef __cplusplus
}
#endif
