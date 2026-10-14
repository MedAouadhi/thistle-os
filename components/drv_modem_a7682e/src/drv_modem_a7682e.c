// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — Simcom A7682E 4G LTE modem driver (esp_modem backend)
//
// Power and timing figures are from the A7682E Hardware Design V1.05, 3.2:
// PWRKEY low for 50 ms (typ) powers the module on and it answers on its UART
// some 8 s later; low for at least 2.5 s powers it off; AT+CPOF shuts it down
// in about 2 s, and its supply must not be cut while it runs.

#include "drv_modem_a7682e.h"
#include "a7682e_sms_validation.h"
#include "a7682e_lifecycle.h"
#include "a7682e_internal.h"
#include "a7682e_parse.h"
#include "a7682e_sms.h"

#include "thistle_expander.h"

#include "esp_modem_api.h"
#include "esp_modem_config.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

static const char *TAG = "a7682e";

/* Default timing constants (milliseconds) --------------------------------- */
#define A7682E_DEFAULT_BAUD       115200
#define A7682E_DEFAULT_AT_TIMEOUT 5000
#define A7682E_SUPPLY_SETTLE_MS   100   ///< Supply on to PWRKEY press: ≥30 ms
#define A7682E_PWRON_PULSE_MS     100   ///< PWRKEY press for power-on: 50 ms typ
#define A7682E_PWROFF_PULSE_MS    3000  ///< PWRKEY press for power-off: ≥2.5 s
#define A7682E_ANSWER_MS          1500  ///< A module already running answers in this
#define A7682E_BOOT_TIMEOUT_MS    15000 ///< After a press: answers on UART in 8 s typ
#define A7682E_SHUTDOWN_MS        3000  ///< After AT+CPOF: 2 s typ to power down
#define A7682E_OFF_ON_MS          2000  ///< Powered down to the next start: ≥2 s (Toff-on)
#define A7682E_ESCAPE_GUARD_MS    1100  ///< Silence around "+++": 1 s
#define A7682E_PPP_TIMEOUT_S      30    ///< Seconds to wait for IP after PPP start
/* How often a modem with no data session is asked for SMS: often without an
 * RI line, rarely with one, in case its pulse was missed. */
#define A7682E_SMS_POLL_MS        15000
#define A7682E_SMS_RI_POLL_MS     120000
/* How often a data session is interrupted to look for SMS when no RI line
 * says one came: each look costs the session a few seconds. */
#define A7682E_SMS_DATA_POLL_MS   (10 * 60 * 1000)
/* RI also rings, for seconds at a time, for a call. Within this long of the
 * last look it does not interrupt a data session again. */
#define A7682E_SMS_RING_QUIET_MS  30000
/* The most stored messages read in one fetch; the rest wait for the next. */
#define A7682E_SMS_FETCH_MAX      32
/* An AT+CMGR answer under AT+CSCS="UCS2": 160 characters as 640 hex digits,
 * and the header. */
#define A7682E_SMS_BUF            1024

/* Driver state ------------------------------------------------------------ */
static struct {
    a7682e_config_t  cfg;
    esp_modem_dce_t *dce;        ///< Data Communication Equipment handle
    esp_netif_t     *ppp_netif;  ///< PPP network interface
    bool             powered_on;
    atomic_bool      ppp_connected;
    char             ppp_ip[16];
    bool             known_off;   ///< Shut down by this driver since boot
    bool             asleep;      ///< Sleeping by AT+CSCLK=1 with DTR held high
    bool             resumed;     ///< resumed_at holds when data mode was last restored
    TickType_t       resumed_at;
    uint32_t         dte_baud;    ///< The rate the SoC's UART runs at
    atomic_bool      sms_ring;    ///< RI fell since the last fetch
    bool             sms_fetched; ///< Fetched since the module last powered on
    TickType_t       sms_fetched_at;
    bool             got_ip_handler_registered;
    bool             lost_ip_handler_registered;
    bool             ppp_status_handler_registered;
} s_modem;

static portMUX_TYPE s_ip_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t sms_configure_locked(void);
static void ri_setup(void);
static void ri_arm(void);
static void destroy_transport_locked(void);

/* =========================================================================
 * Internal — PPP event handler
 * ====================================================================== */

static void ppp_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == IP_EVENT && event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "PPP connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        taskENTER_CRITICAL(&s_ip_lock);
        snprintf(s_modem.ppp_ip, sizeof(s_modem.ppp_ip), IPSTR,
                 IP2STR(&event->ip_info.ip));
        taskEXIT_CRITICAL(&s_ip_lock);
        atomic_store(&s_modem.ppp_connected, true);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGI(TAG, "PPP disconnected");
        atomic_store(&s_modem.ppp_connected, false);
        taskENTER_CRITICAL(&s_ip_lock);
        s_modem.ppp_ip[0] = '\0';
        taskEXIT_CRITICAL(&s_ip_lock);
    }
}

/* lwIP reports a session that dies other than by the network hanging up --
 * the LCP keepalive finding the modem gone, a protocol error, our own stop --
 * only as an error status, never as IP_EVENT_PPP_LOST_IP. */
static void ppp_status_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    esp_netif_t *netif = event_data ? *(esp_netif_t **)event_data : NULL;
    if (event_id <= NETIF_PPP_ERRORNONE || event_id >= NETIF_PP_PHASE_OFFSET
        || netif != s_modem.ppp_netif) {
        return;
    }
    if (atomic_exchange(&s_modem.ppp_connected, false)) {
        ESP_LOGW(TAG, "PPP session ended (status %d)", (int)event_id);
    }
    taskENTER_CRITICAL(&s_ip_lock);
    s_modem.ppp_ip[0] = '\0';
    taskEXIT_CRITICAL(&s_ip_lock);
}

bool a7682e_ppp_up(void)
{
    return atomic_load(&s_modem.ppp_connected);
}

const char *a7682e_ppp_ip(void)
{
    static char ip[16];
    taskENTER_CRITICAL(&s_ip_lock);
    memcpy(ip, s_modem.ppp_ip, sizeof(ip));
    taskEXIT_CRITICAL(&s_ip_lock);
    return ip;
}

/* =========================================================================
 * Internal — the module's supply and power key
 * ====================================================================== */

static bool rail_switchable(void)
{
    return s_modem.cfg.power_expander >= 0;
}

static esp_err_t rail_set(bool on)
{
    if (!rail_switchable()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t ret = thistle_expander_set_level(s_modem.cfg.power_expander,
                                               s_modem.cfg.power_expander_pin,
                                               on ? 1 : 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "supply %s failed: %s", on ? "on" : "off", esp_err_to_name(ret));
    }
    return ret;
}

static bool pwrkey_wired(void)
{
    return s_modem.cfg.pwrkey_expander >= 0 || s_modem.cfg.pin_pwrkey >= 0;
}

static void pwrkey_set(bool pressed)
{
    int level = pressed ? s_modem.cfg.pwrkey_press_level : !s_modem.cfg.pwrkey_press_level;
    if (s_modem.cfg.pwrkey_expander >= 0) {
        thistle_expander_set_level(s_modem.cfg.pwrkey_expander,
                                   s_modem.cfg.pwrkey_expander_pin, level);
    } else if (s_modem.cfg.pin_pwrkey >= 0) {
        gpio_set_level(s_modem.cfg.pin_pwrkey, level);
    }
}

static void pwrkey_press(uint32_t ms)
{
    pwrkey_set(true);
    vTaskDelay(pdMS_TO_TICKS(ms));
    pwrkey_set(false);
}

/* =========================================================================
 * Internal — UART transport and PPP netif
 * ====================================================================== */

/* The WiFi manager brings these up too, but only once the drivers have
 * loaded; the modem may be first. */
static esp_err_t ensure_netif_stack(void)
{
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    return ESP_OK;
}

static void destroy_transport_locked(void)
{
    if (s_modem.got_ip_handler_registered) {
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                                     &ppp_event_handler);
        s_modem.got_ip_handler_registered = false;
    }
    if (s_modem.lost_ip_handler_registered) {
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_LOST_IP,
                                     &ppp_event_handler);
        s_modem.lost_ip_handler_registered = false;
    }
    if (s_modem.ppp_status_handler_registered) {
        esp_event_handler_unregister(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                     &ppp_status_handler);
        s_modem.ppp_status_handler_registered = false;
    }
    if (s_modem.dce) {
        esp_modem_destroy(s_modem.dce);
        s_modem.dce = NULL;
    }
    if (s_modem.ppp_netif) {
        esp_netif_destroy(s_modem.ppp_netif);
        s_modem.ppp_netif = NULL;
    }
    atomic_store(&s_modem.ppp_connected, false);
    s_modem.ppp_ip[0] = '\0';
}

/* The UART, the DCE on it, and the PPP netif the DCE will carry. Opening the
 * UART does not depend on the module being on. */
static esp_err_t create_transport_locked(void)
{
    if (s_modem.dce) {
        return ESP_OK;
    }
    esp_err_t ret = ensure_netif_stack();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "network stack init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.tx_io_num  = s_modem.cfg.pin_tx;
    dte_config.uart_config.rx_io_num  = s_modem.cfg.pin_rx;
    dte_config.uart_config.port_num   = s_modem.cfg.uart_num;
    dte_config.uart_config.baud_rate  = (int)s_modem.cfg.baud_rate;
    /* An answer longer than this buffer is given up on: an SMS read under
     * UCS2 runs to some 850 bytes, against the 512 default. */
    dte_config.dte_buffer_size        = A7682E_SMS_BUF;

    /* PPP's default route priority (20) is below WiFi station's (100), so
     * esp_netif keeps WiFi the default route whenever both are up. */
    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    s_modem.ppp_netif = esp_netif_new(&netif_ppp_config);
    if (!s_modem.ppp_netif) {
        ESP_LOGE(TAG, "failed to create PPP netif");
        return ESP_ERR_NO_MEM;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                                     &ppp_event_handler, NULL);
    if (ret != ESP_OK) {
        destroy_transport_locked();
        return ret;
    }
    s_modem.got_ip_handler_registered = true;
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP,
                                     &ppp_event_handler, NULL);
    if (ret != ESP_OK) {
        destroy_transport_locked();
        return ret;
    }
    s_modem.lost_ip_handler_registered = true;
    ret = esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                     &ppp_status_handler, NULL);
    if (ret != ESP_OK) {
        destroy_transport_locked();
        return ret;
    }
    s_modem.ppp_status_handler_registered = true;

    /* ESP_MODEM_DCE_SIM7600 is the closest supported device type for the
     * A7682E — both are Simcom LTE Cat-1 modems sharing the same AT command
     * set. The APN is set per data session, by esp_modem_set_apn(). */
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG("");
    s_modem.dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600,
                                    &dte_config, &dce_config,
                                    s_modem.ppp_netif);
    if (!s_modem.dce) {
        ESP_LOGE(TAG, "failed to create esp_modem DCE");
        destroy_transport_locked();
        return ESP_FAIL;
    }
    s_modem.dte_baud = s_modem.cfg.baud_rate;
    return ESP_OK;
}

esp_modem_dce_t *a7682e_dce_locked(void)
{
    return s_modem.powered_on ? s_modem.dce : NULL;
}

/* The answer to the exchange in progress. esp_modem_command() takes a plain
 * function, and exchanges are serialised by the driver lock. Sized for the
 * longest answer, an SMS read under UCS2; only tasks touch it. */
static EXT_RAM_BSS_ATTR char s_exchange[A7682E_SMS_BUF];

static esp_err_t exchange_got_line(uint8_t *data, size_t len)
{
    size_t n = len < sizeof(s_exchange) - 1 ? len : sizeof(s_exchange) - 1;
    memcpy(s_exchange, data, n);
    s_exchange[n] = '\0';
    switch (a7682e_final_result(s_exchange)) {
    case 1:  return ESP_OK;
    case -1: return ESP_FAIL;
    default: return ESP_ERR_TIMEOUT;  // keep reading
    }
}

esp_err_t a7682e_exchange_locked(const char *cmd, char *resp, size_t resp_size,
                                 uint32_t timeout_ms)
{
    s_exchange[0] = '\0';
    esp_err_t ret = s_modem.dce
        ? esp_modem_command(s_modem.dce, cmd, exchange_got_line, timeout_ms)
        : ESP_ERR_INVALID_STATE;
    if (resp && resp_size > 0) {
        strlcpy(resp, s_exchange, resp_size);
    }
    return ret;
}

static bool answers_within(uint32_t ms)
{
    TickType_t start = xTaskGetTickCount();
    do {
        if (esp_modem_sync(s_modem.dce) == ESP_OK) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    } while (xTaskGetTickCount() - start < pdMS_TO_TICKS(ms));
    return false;
}

/* A module that was carrying data when the SoC reset without it is still in
 * that session and takes nothing on its UART for a command: a second of
 * silence, "+++", and another second escape it, and ATH ends the call. */
static bool escape_stale_session_locked(void)
{
    char resp[A7682E_AT_BUF];
    vTaskDelay(pdMS_TO_TICKS(A7682E_ESCAPE_GUARD_MS));
    esp_modem_at_raw(s_modem.dce, "+++", resp, "OK", "ERROR",
                     A7682E_ESCAPE_GUARD_MS + 1000);
    if (!answers_within(1000)) {
        return false;
    }
    ESP_LOGI(TAG, "escaped a data session left from before a reset");
    esp_modem_at(s_modem.dce, "ATH", resp, 2000);
    return true;
}

/* The SoC's end of the link. The module's end changes only by AT+IPR, which
 * lasts until it powers down: it powers up at baud_rate. */
static void dte_set_baud_locked(uint32_t baud)
{
    if (baud != 0 && baud != s_modem.dte_baud) {
        uart_set_baudrate(s_modem.cfg.uart_num, baud);
        s_modem.dte_baud = baud;
    }
}

/* A module this driver has not seen start may still be running: left on by
 * a reset of the SoC alone, at the fast rate this driver set, perhaps in a
 * data session, which *in_session (if given) reports. Each rate it may be at
 * is tried, the fast one first. On failure the link is left at the power-up
 * rate. */
static bool find_running_module_locked(uint32_t answer_ms, bool *in_session)
{
    uint32_t rates[2] = { s_modem.cfg.fast_baud_rate, s_modem.cfg.baud_rate };
    if (in_session) {
        *in_session = false;
    }
    for (int i = 0; i < 2; i++) {
        if (rates[i] == 0 || (i == 1 && rates[1] == rates[0])) continue;
        dte_set_baud_locked(rates[i]);
        if (answers_within(answer_ms)) {
            return true;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (rates[i] == 0 || (i == 1 && rates[1] == rates[0])) continue;
        dte_set_baud_locked(rates[i]);
        if (escape_stale_session_locked()) {
            if (in_session) {
                *in_session = true;
            }
            return true;
        }
    }
    dte_set_baud_locked(s_modem.cfg.baud_rate);
    return false;
}

/* Both ends to fast_baud_rate. The module answers AT+IPR at the old rate,
 * then changes. If it does not answer at the new one, both go back. */
static void use_fast_rate_locked(void)
{
    uint32_t fast = s_modem.cfg.fast_baud_rate;
    uint32_t was = s_modem.dte_baud;
    if (fast == 0 || fast == was) {
        return;
    }
    if (esp_modem_set_baud(s_modem.dce, (int)fast) != ESP_OK) {
        ESP_LOGW(TAG, "module refused %lu baud; staying at %lu",
                 (unsigned long)fast, (unsigned long)was);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    dte_set_baud_locked(fast);
    if (answers_within(1000)) {
        ESP_LOGI(TAG, "link at %lu baud", (unsigned long)fast);
        return;
    }
    ESP_LOGW(TAG, "no answer at %lu baud; back to %lu",
             (unsigned long)fast, (unsigned long)was);
    esp_modem_set_baud(s_modem.dce, (int)was);
    vTaskDelay(pdMS_TO_TICKS(20));
    dte_set_baud_locked(was);
    answers_within(1000);
}

/* Echo off, so answers are only answers; verbose errors, so a missing SIM
 * says so; operator names in long form for AT+COPS?. A module that reset
 * itself has lost all three. And the network's time and zone set the
 * module's clock (AT+CTZU=1, which the module keeps). */
void a7682e_configure_locked(void)
{
    char resp[A7682E_AT_BUF];
    esp_modem_at(s_modem.dce, "ATE0", resp, 1000);
    esp_modem_at(s_modem.dce, "AT+CMEE=2", resp, 1000);
    esp_modem_at(s_modem.dce, "AT+COPS=3,0", resp, 1000);
    esp_modem_at(s_modem.dce, "AT+CTZU=1", resp, 1000);
}

/* The module's clock as last read. */
static struct {
    bool    read;
    int64_t utc;
    int     offset_min;
    int64_t read_at_us; /* esp_timer */
} s_clock;
static portMUX_TYPE s_clock_lock = portMUX_INITIALIZER_UNLOCKED;

void a7682e_clock_read_locked(void)
{
    char resp[A7682E_EXCHANGE_BUF];
    int64_t utc;
    int offset;
    if (a7682e_exchange_locked("AT+CCLK?\r", resp, sizeof(resp), 3000) != ESP_OK
        || !a7682e_parse_cclk(resp, &utc, &offset)) {
        return;
    }
    int64_t at = esp_timer_get_time();
    taskENTER_CRITICAL(&s_clock_lock);
    s_clock.read = true;
    s_clock.utc = utc;
    s_clock.offset_min = offset;
    s_clock.read_at_us = at;
    taskEXIT_CRITICAL(&s_clock_lock);
}

void a7682e_clock_read(void)
{
    if (!a7682e_lifecycle_begin_operation()) {
        return;
    }
    if (s_modem.dce && s_modem.powered_on && !s_modem.asleep
        && esp_modem_get_mode(s_modem.dce) != ESP_MODEM_MODE_DATA) {
        a7682e_clock_read_locked();
    }
    a7682e_lifecycle_finish_operation();
}

esp_err_t drv_a7682e_get_network_time(hal_net_time_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_clock_lock);
    bool read = s_clock.read;
    int64_t utc = s_clock.utc;
    int offset = s_clock.offset_min;
    int64_t at = s_clock.read_at_us;
    taskEXIT_CRITICAL(&s_clock_lock);
    if (!read) {
        return ESP_ERR_NOT_FOUND;
    }
    int64_t age_ms = (esp_timer_get_time() - at) / 1000;
    out->utc = utc;
    out->age_ms = age_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)age_ms;
    out->utc_offset_min = (int16_t)offset;
    out->offset_known = true;
    return ESP_OK;
}

esp_err_t a7682e_power_on_locked(void)
{
    a7682e_sleep_locked(false);
    if (s_modem.powered_on && s_modem.dce) {
        return ESP_OK;
    }
    esp_err_t ret = create_transport_locked();
    if (ret != ESP_OK) {
        return ret;
    }
    if (rail_switchable()) {
        ret = rail_set(true);
        if (ret != ESP_OK) {
            destroy_transport_locked();
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(A7682E_SUPPLY_SETTLE_MS));
    }

    /* Only a module that is off gets the key: one that is already running
     * answers, at one rate or the other, or once escaped from a stale
     * session. One this driver shut down is off, and is not looked for. */
    bool in_session = false;
    bool alive = !s_modem.known_off &&
                 find_running_module_locked(A7682E_ANSWER_MS, &in_session);
    if (alive && in_session && pwrkey_wired()) {
        /* Dialled again, a module that kept a session through the SoC's
         * reset stops answering for tens of seconds at a time, at any rate;
         * one started afresh does not (ticket 10). So it is started afresh. */
        ESP_LOGI(TAG, "power-on: restarting the module to end that session");
        a7682e_power_off_locked();
        vTaskDelay(pdMS_TO_TICKS(A7682E_OFF_ON_MS));
        return a7682e_power_on_locked();
    }
    if (!alive && pwrkey_wired()) {
        dte_set_baud_locked(s_modem.cfg.baud_rate);
        ESP_LOGI(TAG, "power-on: pressing PWRKEY for %d ms", A7682E_PWRON_PULSE_MS);
        pwrkey_press(A7682E_PWRON_PULSE_MS);
        alive = answers_within(A7682E_BOOT_TIMEOUT_MS);
    }
    if (!alive) {
        ESP_LOGE(TAG, "power-on: no answer on UART%d (TX=%d RX=%d)",
                 (int)s_modem.cfg.uart_num, (int)s_modem.cfg.pin_tx,
                 (int)s_modem.cfg.pin_rx);
        destroy_transport_locked();
        rail_set(false);
        return ESP_ERR_TIMEOUT;
    }

    a7682e_configure_locked();
    use_fast_rate_locked();
    s_modem.powered_on = true;
    s_modem.known_off = false;
    ESP_LOGI(TAG, "power-on: modem answering");
    /* The module forgets the text mode and storage SMS reading relies on. */
    if (sms_configure_locked() != ESP_OK) {
        ESP_LOGW(TAG, "power-on: SMS settings not applied");
    }
    s_modem.sms_fetched = false;
    ri_arm();
    return ESP_OK;
}

esp_err_t a7682e_power_off_locked(void)
{
    /* A sleeping module answers nothing until DTR wakes it. */
    a7682e_sleep_locked(false);

    if (!s_modem.dce && s_modem.known_off) {
        /* Shut down here already; only the supply could have changed. */
        if (rail_switchable()) {
            rail_set(false);
        }
        return ESP_OK;
    }

    bool was_on = s_modem.powered_on;
    if (!s_modem.dce && rail_switchable()) {
        /* Nothing here has started the module, but a reset of the SoC alone
         * leaves it running on a supply the expander still holds on: it must
         * be shut down before that supply goes. */
        create_transport_locked();
    }

    if (s_modem.dce) {
        a7682e_enter_command_locked();
        bool alive = s_modem.powered_on
            ? answers_within(A7682E_ANSWER_MS) || escape_stale_session_locked()
            : find_running_module_locked(500, NULL);
        if (alive) {
            char resp[A7682E_AT_BUF];
            if (esp_modem_at(s_modem.dce, "AT+CPOF", resp, 3000) == ESP_OK) {
                ESP_LOGI(TAG, "power-off: AT+CPOF accepted");
                vTaskDelay(pdMS_TO_TICKS(A7682E_SHUTDOWN_MS));
            } else if (!rail_switchable() && pwrkey_wired()) {
                /* The supply cannot be cut, so the key is the only way. */
                ESP_LOGW(TAG, "power-off: AT+CPOF refused, holding PWRKEY");
                pwrkey_press(A7682E_PWROFF_PULSE_MS);
                vTaskDelay(pdMS_TO_TICKS(A7682E_SHUTDOWN_MS));
            }
        } else if (was_on && pwrkey_wired()) {
            /* Running but deaf: the key still shuts it down cleanly, which
             * cutting the supply under it would not. */
            ESP_LOGW(TAG, "power-off: no answer, holding PWRKEY");
            pwrkey_press(A7682E_PWROFF_PULSE_MS);
            vTaskDelay(pdMS_TO_TICKS(A7682E_SHUTDOWN_MS));
        }
    }

    destroy_transport_locked();
    if (rail_switchable()) {
        rail_set(false);
    }
    if (s_modem.powered_on) {
        ESP_LOGI(TAG, "power-off: done");
    }
    s_modem.powered_on = false;
    s_modem.known_off = true;
    return ESP_OK;
}

/* AT+CSCLK=1 lets the module sleep whenever DTR is high and it has nothing to
 * do, still paging the network: an SMS pulls RI low for 120 ms, a call holds
 * it low while it rings (A76XX Series Sleep Mode Application Note V1.03,
 * 2.1 and 3.2; LTE sleep, typ. 2 mA against 12 mA idle, A7682E Hardware
 * Design V1.05, table 35). A data session keeps it awake, so it ends first.
 * DTR is held, so it stays high through the SoC's own sleep, deep or light. */
esp_err_t a7682e_sleep_locked(bool enter)
{
    gpio_num_t dtr = s_modem.cfg.pin_dtr;
    if (dtr < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (enter) {
        if (s_modem.asleep) {
            return ESP_OK;
        }
        if (!s_modem.powered_on || !s_modem.dce) {
            return ESP_ERR_INVALID_STATE;
        }
        a7682e_enter_command_locked();
        char resp[A7682E_AT_BUF];
        if (esp_modem_at(s_modem.dce, "AT+CSCLK=1", resp, 2000) != ESP_OK) {
            ESP_LOGW(TAG, "sleep: AT+CSCLK=1 refused; the module stays awake");
            return ESP_FAIL;
        }
        gpio_set_level(dtr, 1);
        gpio_hold_en(dtr);
        s_modem.asleep = true;
        ESP_LOGI(TAG, "sleep: module asleep, RI raised for calls and SMS");
        return ESP_OK;
    }

    if (!s_modem.asleep) {
        return ESP_OK;
    }
    gpio_hold_dis(dtr);
    gpio_set_level(dtr, 0);
    s_modem.asleep = false;
    bool alive = s_modem.dce && answers_within(A7682E_ANSWER_MS);
    if (alive) {
        char resp[A7682E_AT_BUF];
        esp_modem_at(s_modem.dce, "AT+CSCLK=0", resp, 1000);
        ESP_LOGI(TAG, "sleep: module awake");
    } else {
        ESP_LOGW(TAG, "sleep: module not answering after DTR went low");
    }
    return alive ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool a7682e_enter_command_locked(void)
{
    if (!s_modem.dce || esp_modem_get_mode(s_modem.dce) != ESP_MODEM_MODE_DATA) {
        return false;
    }
    esp_err_t ret = esp_modem_set_mode(s_modem.dce, ESP_MODEM_MODE_COMMAND);
    if (ret != ESP_OK) {
        /* The DCE is left undefined, from which data mode can still be set. */
        ESP_LOGW(TAG, "leaving data mode: %s", esp_err_to_name(ret));
    }
    atomic_store(&s_modem.ppp_connected, false);
    return true;
}

void a7682e_restore_data_locked(bool was_data)
{
    if (!was_data || !s_modem.dce) {
        return;
    }
    esp_err_t ret = esp_modem_set_mode(s_modem.dce, ESP_MODEM_MODE_DATA);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "could not resume data mode (%s); it will be redialled",
                 esp_err_to_name(ret));
        return;
    }
    s_modem.resumed = true;
    s_modem.resumed_at = xTaskGetTickCount();
}

bool a7682e_resumed_within_locked(uint32_t ms)
{
    return s_modem.resumed && xTaskGetTickCount() - s_modem.resumed_at < pdMS_TO_TICKS(ms);
}

/* =========================================================================
 * Public API — Lifecycle
 * ====================================================================== */

esp_err_t drv_a7682e_init(const a7682e_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    a7682e_init_result_t begin = a7682e_lifecycle_begin_init();
    if (begin == A7682E_INIT_ALREADY_RUNNING) {
        return ESP_OK;
    }
    if (begin != A7682E_INIT_ACQUIRED) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(&s_modem.cfg, config, sizeof(a7682e_config_t));

    /* Apply default baud rate */
    if (s_modem.cfg.baud_rate == 0) {
        s_modem.cfg.baud_rate = A7682E_DEFAULT_BAUD;
    }

    /* PWRKEY released, RESET de-asserted (active-low), DTR low so the module
     * never sleeps on the SoC. The supply is left as it is: only a module
     * known to be shut down may lose it. */
    if (s_modem.cfg.pwrkey_expander < 0 && s_modem.cfg.pin_pwrkey >= 0) {
        gpio_set_direction(s_modem.cfg.pin_pwrkey, GPIO_MODE_OUTPUT);
    }
    pwrkey_set(false);
    if (s_modem.cfg.pin_reset >= 0) {
        gpio_set_direction(s_modem.cfg.pin_reset, GPIO_MODE_OUTPUT);
        gpio_set_level(s_modem.cfg.pin_reset, 1);
    }
    if (s_modem.cfg.pin_dtr >= 0) {
        /* Held high through a deep sleep the module slept in: let go, or the
         * pin would not move, and low wakes the module. */
        gpio_hold_dis(s_modem.cfg.pin_dtr);
        gpio_set_direction(s_modem.cfg.pin_dtr, GPIO_MODE_OUTPUT);
        gpio_set_level(s_modem.cfg.pin_dtr, 0);
    }
    s_modem.asleep = false;
    ri_setup();

    atomic_store(&s_modem.ppp_connected, false);
    ESP_LOGI(TAG, "driver initialised (esp_modem backend, UART%d, %lu baud then %lu, "
             "TX=%d RX=%d PWRKEY=%d/exp%d:%d RST=%d DTR=%d RI=%d supply=exp%d:%d)",
             (int)s_modem.cfg.uart_num, (unsigned long)s_modem.cfg.baud_rate,
             (unsigned long)s_modem.cfg.fast_baud_rate,
             (int)s_modem.cfg.pin_tx, (int)s_modem.cfg.pin_rx,
             (int)s_modem.cfg.pin_pwrkey, s_modem.cfg.pwrkey_expander,
             s_modem.cfg.pwrkey_expander_pin, (int)s_modem.cfg.pin_reset,
             (int)s_modem.cfg.pin_dtr, (int)s_modem.cfg.pin_ri, s_modem.cfg.power_expander,
             s_modem.cfg.power_expander_pin);
    a7682e_lifecycle_finish_init(true);
    return ESP_OK;
}

void drv_a7682e_deinit(void)
{
    if (!a7682e_lifecycle_begin_stop()) {
        return;
    }

    a7682e_power_off_locked();

    /* Release GPIO */
    if (s_modem.cfg.pwrkey_expander < 0 && s_modem.cfg.pin_pwrkey >= 0) {
        gpio_reset_pin(s_modem.cfg.pin_pwrkey);
    }
    if (s_modem.cfg.pin_reset >= 0) {
        gpio_reset_pin(s_modem.cfg.pin_reset);
    }
    if (s_modem.cfg.pin_dtr >= 0) {
        gpio_reset_pin(s_modem.cfg.pin_dtr);
    }

    if (s_modem.cfg.pin_ri >= 0) {
        gpio_isr_handler_remove(s_modem.cfg.pin_ri);
    }
    ESP_LOGI(TAG, "driver de-initialised");
    a7682e_lifecycle_finish_stop();
}

/* =========================================================================
 * Public API — Power control
 * ====================================================================== */

esp_err_t drv_a7682e_power(bool on)
{
    if (!a7682e_lifecycle_begin_operation()) {
        ESP_LOGE(TAG, "power: driver not initialised");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = on ? a7682e_power_on_locked() : a7682e_power_off_locked();
    a7682e_lifecycle_finish_operation();
    return result;
}

/* =========================================================================
 * Public API — AT command interface
 * ====================================================================== */

esp_err_t drv_a7682e_send_at(const char *cmd, char *buf, size_t buf_len,
                              uint32_t timeout_ms)
{
    if (!cmd) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!a7682e_lifecycle_begin_operation()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_modem.dce || !s_modem.powered_on) {
        ESP_LOGE(TAG, "send_at: modem not powered on");
        a7682e_lifecycle_finish_operation();
        return ESP_ERR_INVALID_STATE;
    }

    if (timeout_ms == 0) {
        timeout_ms = A7682E_DEFAULT_AT_TIMEOUT;
    }

    bool was_data = a7682e_enter_command_locked();
    char resp[A7682E_AT_BUF] = {0};
    esp_err_t ret = esp_modem_at(s_modem.dce, cmd, resp, (int)timeout_ms);
    a7682e_restore_data_locked(was_data);

    if (buf && buf_len > 0) {
        strncpy(buf, resp, buf_len - 1);
        buf[buf_len - 1] = '\0';
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "send_at: '%s' returned %s", cmd, esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "AT<< %s", resp);
    }

    a7682e_lifecycle_finish_operation();
    return ret;
}

bool drv_a7682e_is_ready(void)
{
    if (!a7682e_lifecycle_begin_operation()) {
        return false;
    }
    bool ready = s_modem.powered_on && s_modem.dce;
    a7682e_lifecycle_finish_operation();
    return ready;
}

/* =========================================================================
 * Public API — Network helpers
 * ====================================================================== */

int drv_a7682e_get_signal_rssi(void)
{
    if (!a7682e_lifecycle_begin_operation()) {
        return -999;
    }
    if (!s_modem.dce || !s_modem.powered_on
        || esp_modem_get_mode(s_modem.dce) == ESP_MODEM_MODE_DATA) {
        a7682e_lifecycle_finish_operation();
        return -999;
    }

    int rssi = 0, ber = 0;
    esp_err_t ret = esp_modem_get_signal_quality(s_modem.dce, &rssi, &ber);
    if (ret != ESP_OK) {
        a7682e_lifecycle_finish_operation();
        return -999;
    }

    if (rssi == 99) {
        /* 99 = not detectable / unknown */
        a7682e_lifecycle_finish_operation();
        return -999;
    }

    /* Convert to dBm: dBm = −113 + 2×rssi (range 0–31 → −113 to −51 dBm) */
    int dbm = -113 + 2 * rssi;
    ESP_LOGD(TAG, "signal: rssi=%d → %d dBm", rssi, dbm);
    a7682e_lifecycle_finish_operation();
    return dbm;
}

a7682e_net_reg_t drv_a7682e_get_network_reg(void)
{
    if (!a7682e_lifecycle_begin_operation()) {
        return A7682E_NET_UNKNOWN;
    }
    if (!s_modem.dce || !s_modem.powered_on
        || esp_modem_get_mode(s_modem.dce) == ESP_MODEM_MODE_DATA) {
        a7682e_lifecycle_finish_operation();
        return A7682E_NET_UNKNOWN;
    }

    char resp[A7682E_AT_BUF] = {0};
    if (esp_modem_at(s_modem.dce, "AT+CREG?", resp, 2000) != ESP_OK) {
        a7682e_lifecycle_finish_operation();
        return A7682E_NET_UNKNOWN;
    }

    /* Response format: "+CREG: <n>,<stat>" or "+CREG: <stat>" */
    const char *p = strstr(resp, "+CREG:");
    if (!p) {
        ESP_LOGW(TAG, "get_network_reg: could not parse +CREG in: %s", resp);
        a7682e_lifecycle_finish_operation();
        return A7682E_NET_UNKNOWN;
    }

    int n = 0, stat = 0;
    int parsed = sscanf(p, "+CREG: %d,%d", &n, &stat);
    if (parsed == 2) {
        /* n,stat form — stat is the registration status */
    } else if (parsed == 1) {
        /* Single-field form — the value is stat */
        stat = n;
    } else {
        ESP_LOGW(TAG, "get_network_reg: sscanf failed on: %s", p);
        a7682e_lifecycle_finish_operation();
        return A7682E_NET_UNKNOWN;
    }

    a7682e_net_reg_t result;
    switch (stat) {
        case 0:  result = A7682E_NET_NOT_REGISTERED; break;
        case 1:  result = A7682E_NET_REGISTERED_HOME; break;
        case 2:  result = A7682E_NET_SEARCHING; break;
        case 3:  result = A7682E_NET_DENIED; break;
        case 5:  result = A7682E_NET_REGISTERED_ROAM; break;
        default: result = A7682E_NET_UNKNOWN; break;
    }
    a7682e_lifecycle_finish_operation();
    return result;
}

/* =========================================================================
 * Public API — PPP data connection
 * ====================================================================== */

esp_err_t drv_a7682e_start_ppp(void)
{
    if (!a7682e_lifecycle_begin_operation()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_modem.dce || !s_modem.powered_on) {
        ESP_LOGE(TAG, "start_ppp: modem not powered on");
        a7682e_lifecycle_finish_operation();
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load(&s_modem.ppp_connected)) {
        a7682e_lifecycle_finish_operation();
        return ESP_OK;
    }

    /* A session that dropped leaves the DCE in data mode, from which data
     * mode cannot be entered again. */
    a7682e_enter_command_locked();

    ESP_LOGI(TAG, "start_ppp: switching to PPP data mode...");

    /* esp_modem sends AT+CGDCONT with the APN, then ATD*99# */
    esp_err_t ret = esp_modem_set_mode(s_modem.dce, ESP_MODEM_MODE_DATA);
    a7682e_lifecycle_finish_operation();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start_ppp: failed to enter PPP mode: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /* Wait for IP_EVENT_PPP_GOT_IP (event handler sets ppp_connected),
     * without holding the driver: SMS and status calls go on meanwhile. */
    for (int i = 0; i < A7682E_PPP_TIMEOUT_S
         && !atomic_load(&s_modem.ppp_connected); i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!atomic_load(&s_modem.ppp_connected)) {
        ESP_LOGW(TAG, "start_ppp: timed out waiting for IP address");
        if (a7682e_lifecycle_begin_operation()) {
            a7682e_enter_command_locked();
            a7682e_lifecycle_finish_operation();
        }
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "start_ppp: PPP up — TCP/IP stack routed through 4G");
    return ESP_OK;
}

esp_err_t drv_a7682e_stop_ppp(void)
{
    if (!a7682e_lifecycle_begin_operation()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_modem.dce) {
        a7682e_lifecycle_finish_operation();
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "stop_ppp: returning to AT command mode");
    a7682e_enter_command_locked();
    a7682e_lifecycle_finish_operation();
    return ESP_OK;
}

bool drv_a7682e_ppp_connected(void)
{
    return atomic_load(&s_modem.ppp_connected);
}

/* =========================================================================
 * Public API — Legacy stubs (superseded by PPP + standard networking)
 * ====================================================================== */

esp_err_t drv_a7682e_connect_tcp(const char *host, uint16_t port)
{
    (void)host;
    (void)port;
    /* With PPP active, use standard lwIP sockets or esp_http_client instead
     * of raw AT+CIPSTART commands. */
    ESP_LOGW(TAG, "connect_tcp: use lwIP sockets with PPP active");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t drv_a7682e_send_data(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    ESP_LOGW(TAG, "send_data: use standard sockets with PPP active");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t drv_a7682e_http_get(const char *url, char *buf, size_t buf_len)
{
    (void)url;
    (void)buf;
    (void)buf_len;
    /* With PPP active, esp_http_client routes through the PPP netif
     * automatically — no driver-level AT+HTTPACTION needed. */
    ESP_LOGW(TAG, "http_get: use esp_http_client with PPP active "
             "(works just like WiFi)");
    return ESP_ERR_NOT_SUPPORTED;
}

/* =========================================================================
 * Public API — SMS
 * ====================================================================== */

/* The module pulls RI low for 120 ms when an SMS arrives, whatever its UART
 * is doing, and for seconds at a time while a call rings (A76XX Series Sleep
 * Mode Application Note V1.03, 3.2). The pin is an input with a pull-up:
 * it floats while the module's level shifter is off. */
static void IRAM_ATTR ri_isr(void *arg)
{
    (void)arg;
    atomic_store(&s_modem.sms_ring, true);
}

static void ri_setup(void)
{
    gpio_num_t ri = s_modem.cfg.pin_ri;
    if (ri < 0) {
        return;
    }
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << ri,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    esp_err_t ret = gpio_config(&io);
    if (ret == ESP_OK) {
        ret = gpio_install_isr_service(0);
        if (ret == ESP_ERR_INVALID_STATE) {
            ret = ESP_OK;  // installed by someone else first
        }
    }
    if (ret == ESP_OK) {
        ret = gpio_isr_handler_add(ri, ri_isr, NULL);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "RI on GPIO%d not watched: %s", (int)ri, esp_err_to_name(ret));
        s_modem.cfg.pin_ri = GPIO_NUM_NC;
    }
}

/* The power manager configures the same pin as a wake source, with its
 * interrupt off, and may do so after the module powered on: each power-on,
 * and each look at whether a fetch is due, turns it back on. */
static void ri_arm(void)
{
    if (s_modem.cfg.pin_ri >= 0) {
        gpio_set_intr_type(s_modem.cfg.pin_ri, GPIO_INTR_NEGEDGE);
        gpio_intr_enable(s_modem.cfg.pin_ri);
    }
}

/* Text mode with every header field (AT+CSDH=1 gives the first octet and
 * the coding scheme the text is read by), messages kept in the module's own
 * storage, and +CMTI notices, which are what pulse RI. */
static esp_err_t sms_configure_locked(void)
{
    char resp[A7682E_AT_BUF] = {0};

    esp_err_t ret = esp_modem_at(s_modem.dce, "AT+CMGF=1", resp, 2000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sms: AT+CMGF=1 failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_modem_at(s_modem.dce, "AT+CSDH=1", resp, 2000);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "sms: AT+CSDH=1 failed: %s", esp_err_to_name(ret));
    }
    ret = esp_modem_at(s_modem.dce, "AT+CPMS=\"ME\",\"ME\",\"ME\"", resp, 2000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sms: AT+CPMS failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_modem_at(s_modem.dce, "AT+CNMI=2,1,0,0,0", resp, 2000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sms: AT+CNMI failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

bool drv_a7682e_sms_due(void)
{
    if (!a7682e_lifecycle_begin_operation()) {
        return false;
    }
    bool due = false;
    if (s_modem.powered_on && s_modem.dce && !s_modem.asleep) {
        ri_arm();
        bool data = esp_modem_get_mode(s_modem.dce) == ESP_MODEM_MODE_DATA;
        bool ring = atomic_load(&s_modem.sms_ring);
        bool has_ri = s_modem.cfg.pin_ri >= 0;
        TickType_t since = xTaskGetTickCount() - s_modem.sms_fetched_at;
        if (!s_modem.sms_fetched) {
            /* What came while the module was off or the SoC was not looking.
             * Not in a session that just started: it is fetched on the next
             * ring, or the next session's start. */
            due = !data;
        } else if (data) {
            due = has_ri
                ? ring && since >= pdMS_TO_TICKS(A7682E_SMS_RING_QUIET_MS)
                : since >= pdMS_TO_TICKS(A7682E_SMS_DATA_POLL_MS);
        } else {
            due = ring || since >= pdMS_TO_TICKS(has_ri ? A7682E_SMS_RI_POLL_MS
                                                        : A7682E_SMS_POLL_MS);
        }
    }
    a7682e_lifecycle_finish_operation();
    return due;
}

/* Read every message the storage holds, hand each to cb, and delete those cb
 * kept. AT+CMGL would list them in one answer, but marks each it lists read,
 * so one that did not fit the answer would never be listed again; AT+CMGD=?
 * names where they are, and each is read on its own. */
static esp_err_t sms_fetch_locked(hal_sms_rx_cb_t cb, void *user_data, int *kept)
{
    struct scratch {
        char      resp[A7682E_SMS_BUF];
        int       index[A7682E_SMS_FETCH_MAX];
        hal_sms_t sms;
    };
    struct scratch *w = heap_caps_malloc(sizeof(*w), MALLOC_CAP_SPIRAM);
    if (!w) {
        w = malloc(sizeof(*w));
    }
    if (!w) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = a7682e_exchange_locked("AT+CPMS?\r", w->resp, sizeof(w->resp), 5000);
    int used = ret == ESP_OK ? a7682e_parse_storage_used(w->resp) : -1;
    if (used <= 0) {
        free(w);
        return used == 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }

    /* Under UCS2 the sender and the text of a 7-bit message come as UCS2 in
     * hex, so an a-umlaut or a euro sign arrives as itself, and the line
     * breaks inside a message cannot be taken for the end of the answer. */
    bool ucs2 = a7682e_exchange_locked("AT+CSCS=\"UCS2\"\r", NULL, 0, 2000) == ESP_OK;

    ret = a7682e_exchange_locked("AT+CMGD=?\r", w->resp, sizeof(w->resp), 5000);
    int count = ret == ESP_OK
        ? a7682e_parse_index_list(w->resp, w->index, A7682E_SMS_FETCH_MAX) : -1;
    if (count < 0) {
        ESP_LOGW(TAG, "sms: %d stored, but AT+CMGD=? did not say where", used);
        ret = ESP_ERR_INVALID_RESPONSE;
    } else {
        ret = ESP_OK;
    }

    int found = 0;
    for (int i = 0; i < count && found < used; i++) {
        char cmd[24];
        snprintf(cmd, sizeof(cmd), "AT+CMGR=%d\r", w->index[i]);
        if (a7682e_exchange_locked(cmd, w->resp, sizeof(w->resp), 5000) != ESP_OK) {
            continue;  // an empty slot, if AT+CMGD=? lists every one
        }
        esp_err_t got = a7682e_parse_cmgr(w->resp, ucs2, &w->sms);
        if (got == ESP_ERR_INVALID_RESPONSE) {
            ESP_LOGW(TAG, "sms: message %d unreadable; left in storage", w->index[i]);
            continue;
        }
        found++;
        /* A status report, or a message stored to be sent: nothing here asks
         * for either, and left, it would be read at every fetch. */
        bool drop = got == ESP_ERR_NOT_SUPPORTED;
        if (!drop && cb(&w->sms, user_data)) {
            (*kept)++;
            drop = true;
        }
        if (drop) {
            snprintf(cmd, sizeof(cmd), "AT+CMGD=%d\r", w->index[i]);
            if (a7682e_exchange_locked(cmd, NULL, 0, 5000) != ESP_OK) {
                ESP_LOGW(TAG, "sms: could not delete message %d", w->index[i]);
            }
        }
    }

    /* AT+CMGS takes the number in the character set in force. */
    if (ucs2) {
        a7682e_exchange_locked("AT+CSCS=\"IRA\"\r", NULL, 0, 2000);
    }
    free(w);
    return ret;
}

esp_err_t drv_a7682e_sms_fetch(hal_sms_rx_cb_t cb, void *user_data, int *kept)
{
    int none = 0;
    if (!kept) {
        kept = &none;
    }
    *kept = 0;
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!a7682e_lifecycle_begin_operation()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_modem.dce || !s_modem.powered_on || s_modem.asleep) {
        a7682e_lifecycle_finish_operation();
        return ESP_ERR_INVALID_STATE;
    }

    /* Cleared before the look, so a message arriving during it rings again. */
    atomic_store(&s_modem.sms_ring, false);
    bool was_data = a7682e_enter_command_locked();
    esp_err_t ret = sms_fetch_locked(cb, user_data, kept);
    a7682e_clock_read_locked();
    a7682e_restore_data_locked(was_data);
    s_modem.sms_fetched = true;
    s_modem.sms_fetched_at = xTaskGetTickCount();
    a7682e_lifecycle_finish_operation();

    if (*kept > 0) {
        ESP_LOGI(TAG, "sms: %d received", *kept);
    }
    return ret;
}

esp_err_t drv_a7682e_send_sms(const char *phone, const char *msg)
{
    if (!a7682e_sms_phone_is_valid(phone)
        || !a7682e_sms_message_is_valid(msg)) {
        ESP_LOGE(TAG, "send_sms: rejected unsafe recipient or message text");
        return ESP_ERR_INVALID_ARG;
    }
    if (!a7682e_lifecycle_begin_operation()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_modem.dce || !s_modem.powered_on) {
        ESP_LOGE(TAG, "send_sms: modem not powered on");
        a7682e_lifecycle_finish_operation();
        return ESP_ERR_INVALID_STATE;
    }

    bool was_data = a7682e_enter_command_locked();

    /* Ensure text mode is active before sending */
    esp_err_t ret = esp_modem_sms_txt_mode(s_modem.dce, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send_sms: failed to enable text mode: %s",
                 esp_err_to_name(ret));
        goto restore;
    }
    /* The number and the text go as they are, which a fetch left in UCS2
     * would not take. */
    a7682e_exchange_locked("AT+CSCS=\"IRA\"\r", NULL, 0, 2000);

    ESP_LOGI(TAG, "send_sms: sending to %s", phone);
    /* esp_modem_send_sms performs the prompt-aware two-stage exchange: it
     * submits the validated recipient command, waits for the CMGS prompt, and
     * only then transmits the validated message body and terminator. */
    ret = esp_modem_send_sms(s_modem.dce, phone, msg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send_sms: failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "send_sms: sent successfully");
    }

restore:
    a7682e_restore_data_locked(was_data);
    a7682e_lifecycle_finish_operation();
    return ret;
}
