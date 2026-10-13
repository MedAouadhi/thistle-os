// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — the A7682E as a cellular network transport (hal_net_driver_t)
//
// connect() walks the module from off to carrying data: power, a ready SIM,
// a registration, then a PPP session. Each step holds the driver only for its
// own exchange with the module, so SMS and status calls are not shut out for
// the minute a registration can take, and disconnect() can cut in between.

#include "drv_modem_a7682e.h"
#include "a7682e_internal.h"
#include "a7682e_lifecycle.h"
#include "a7682e_parse.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <string.h>

static const char *TAG = "a7682e_net";

#define CONNECT_DEFAULT_TIMEOUT_MS 120000
#define SIM_POLL_MS                1000
#define SIM_SETTLE_POLLS           4
#define REG_POLL_MS                2000
#define PPP_TIMEOUT_MS             30000
/* An SMS exchange leaves data mode and redials; the address is back within
 * a few seconds. */
#define RESUME_WAIT_MS             15000
/* A module that stops answering mid-connect is wedged; it is powered off so
 * the next attempt starts it from cold. */
#define MAX_SILENT_POLLS           3

static atomic_int  s_state = HAL_NET_STATE_DISCONNECTED;
static atomic_bool s_abort;
/* A session was up and has not been ended by disconnect(): when it is found
 * dead, the module is asked why. */
static atomic_bool s_had_session;
static hal_cell_info_t s_info;
static portMUX_TYPE s_info_lock = portMUX_INITIALIZER_UNLOCKED;

static void info_update(void (*apply)(hal_cell_info_t *, const void *), const void *arg)
{
    taskENTER_CRITICAL(&s_info_lock);
    apply(&s_info, arg);
    taskEXIT_CRITICAL(&s_info_lock);
}

static void apply_powered(hal_cell_info_t *info, const void *arg)
{
    bool powered = *(const bool *)arg;
    if (!powered) {
        memset(info, 0, sizeof(*info));
    }
    info->powered = powered;
}

static void apply_sim(hal_cell_info_t *info, const void *arg)
{
    info->sim = *(const hal_sim_state_t *)arg;
}

static void apply_network(hal_cell_info_t *info, const void *arg)
{
    const hal_cell_info_t *seen = arg;
    info->reg = seen->reg;
    info->rssi_dbm = seen->rssi_dbm;
    memcpy(info->operator_name, seen->operator_name, sizeof(info->operator_name));
}

static bool deadline_passed(TickType_t start, uint32_t timeout_ms)
{
    return xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms);
}

/* Sleep in short steps so cancel() or disconnect() is heard within a quarter
 * second. */
static bool sleep_unless_aborted(uint32_t ms)
{
    for (uint32_t slept = 0; slept < ms; slept += 250) {
        if (atomic_load(&s_abort)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return !atomic_load(&s_abort);
}

/* -------------------------------------------------------------------------
 * One exchange each, under the driver
 * ---------------------------------------------------------------------- */

/* disconnect() may have run while this attempt waited for the driver: the
 * steps that start something check for it once they hold the driver. */
static esp_err_t power_on(void)
{
    if (!a7682e_lifecycle_begin_operation()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = atomic_load(&s_abort) ? ESP_ERR_TIMEOUT : a7682e_power_on_locked();
    a7682e_lifecycle_finish_operation();
    return ret;
}

/* The DCE is carrying data, or redialling the session an SMS exchange
 * interrupted. Commands sent now would go into the PPP stream. */
static bool in_data_mode_locked(esp_modem_dce_t *dce)
{
    return dce && esp_modem_get_mode(dce) == ESP_MODEM_MODE_DATA;
}

/* An SMS exchange put the DCE back into data mode moments ago, and the
 * session it interrupted is still coming back. */
static bool resuming(void)
{
    if (!a7682e_lifecycle_begin_operation()) {
        return false;
    }
    bool resuming = in_data_mode_locked(a7682e_dce_locked())
                    && a7682e_resumed_within_locked(RESUME_WAIT_MS);
    a7682e_lifecycle_finish_operation();
    return resuming;
}

static void flatten(char *s)
{
    for (; *s; s++) {
        if (*s == '\r' || *s == '\n') *s = ' ';
    }
}

/* A session that dropped leaves the DCE in data mode with no link; commands
 * need it back in command mode, and a module that reset itself needs its
 * echo and error settings again. */
static void leave_dead_session(void)
{
    if (!a7682e_lifecycle_begin_operation()) {
        return;
    }
    if (!a7682e_ppp_up()) {
        a7682e_enter_command_locked();
    }
    if (a7682e_dce_locked() && !in_data_mode_locked(a7682e_dce_locked())) {
        a7682e_configure_locked();
        if (atomic_exchange(&s_had_session, false)) {
            char resp[A7682E_AT_BUF] = {0};
            a7682e_exchange_locked("AT+CEER\r", resp, sizeof(resp), 2000);
            flatten(resp);
            ESP_LOGW(TAG, "the last session ended; the module reports: %s", resp);
        }
    }
    a7682e_lifecycle_finish_operation();
}

/* HAL_SIM_UNKNOWN with *answered false when the module said nothing. A
 * state other than ready is logged with the module's own words, once per
 * change. */
static hal_sim_state_t read_sim(bool *answered)
{
    static hal_sim_state_t last_logged = HAL_SIM_READY;
    *answered = false;
    if (!a7682e_lifecycle_begin_operation()) {
        return HAL_SIM_UNKNOWN;
    }
    hal_sim_state_t sim = HAL_SIM_UNKNOWN;
    esp_modem_dce_t *dce = a7682e_dce_locked();
    if (in_data_mode_locked(dce)) {
        *answered = true;  // busy carrying data, not silent
    } else if (dce) {
        /* The whole answer: a missing SIM is told by the error line. */
        char resp[A7682E_EXCHANGE_BUF] = {0};
        a7682e_exchange_locked("AT+CPIN?\r", resp, sizeof(resp), 9000);
        *answered = resp[0] != '\0';
        sim = a7682e_parse_sim(resp);
        if (sim != HAL_SIM_READY && sim != last_logged) {
            flatten(resp);
            ESP_LOGI(TAG, "SIM state %d: %s", (int)sim, resp);
        }
        last_logged = sim;
    }
    a7682e_lifecycle_finish_operation();
    return sim;
}

static bool read_network(hal_cell_info_t *seen)
{
    memset(seen, 0, sizeof(*seen));
    if (!a7682e_lifecycle_begin_operation()) {
        return false;
    }
    esp_modem_dce_t *dce = a7682e_dce_locked();
    bool answered = in_data_mode_locked(dce);
    if (dce && !answered) {
        char resp[A7682E_AT_BUF];
        hal_cell_reg_t reg = HAL_CELL_REG_UNKNOWN;
        /* LTE first, then the GPRS fallback the A7682E also has. CREG is the
         * circuit-switched domain, which carries no data. */
        if (esp_modem_at(dce, "AT+CEREG?", resp, 2000) == ESP_OK) {
            answered = true;
            reg = a7682e_parse_reg(resp);
        }
        if (esp_modem_at(dce, "AT+CGREG?", resp, 2000) == ESP_OK) {
            answered = true;
            reg = a7682e_reg_best(reg, a7682e_parse_reg(resp));
        }
        seen->reg = reg;

        int rssi = 99, ber = 99;
        if (esp_modem_get_signal_quality(dce, &rssi, &ber) == ESP_OK && rssi != 99) {
            seen->rssi_dbm = (int16_t)(-113 + 2 * rssi);
        }
        if (a7682e_reg_is_registered(reg)
            && esp_modem_at(dce, "AT+COPS?", resp, 3000) == ESP_OK) {
            a7682e_parse_operator(resp, seen->operator_name, sizeof(seen->operator_name));
        }
    }
    a7682e_lifecycle_finish_operation();
    return answered;
}

/* The APN goes inside a quoted AT+CGDCONT argument: labels of letters,
 * digits and hyphens joined by dots, and the underscores some carriers use. */
static bool apn_is_valid(const char *apn)
{
    size_t len = 0;
    for (const char *c = apn; *c; c++, len++) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')
                  || (*c >= '0' && *c <= '9') || *c == '.' || *c == '-' || *c == '_';
        if (!ok || len >= 63) {
            return false;
        }
    }
    return true;
}

static esp_err_t dial(const char *apn)
{
    if (apn && !apn_is_valid(apn)) {
        ESP_LOGE(TAG, "refusing an APN with characters AT+CGDCONT cannot carry");
        return ESP_ERR_INVALID_ARG;
    }
    if (!a7682e_lifecycle_begin_operation()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    esp_modem_dce_t *dce = atomic_load(&s_abort) ? NULL : a7682e_dce_locked();
    if (dce && a7682e_ppp_up()) {
        ret = ESP_OK;  // an SMS exchange's redial got there first
    } else if (dce) {
        a7682e_enter_command_locked();
        /* Sent as AT+CGDCONT=1,"IP","<apn>" when data mode is entered.
         * Context 1 is the default bearer, up since the attach, and the
         * module reports the APN the network gave it. Rewritten with an empty
         * APN instead, the first session after each power-on was ended by
         * the module or went silent, eight times in nine; written back as
         * the module reports it, eight sessions in eight held (ticket 10).
         * AT+CGDCONT is saved, so the module asks for this APN from then on;
         * an empty one written while attached does not change what it
         * reports. */
        char network_apn[65] = "";
        if (!apn || !apn[0]) {
            char resp[A7682E_EXCHANGE_BUF] = {0};
            a7682e_exchange_locked("AT+CGDCONT?\r", resp, sizeof(resp), 3000);
            if (!a7682e_parse_context_apn(resp, 1, network_apn, sizeof(network_apn))
                || !apn_is_valid(network_apn)) {
                network_apn[0] = '\0';
            }
        }
        const char *use = apn && apn[0] ? apn : network_apn;
        esp_modem_set_apn(dce, use);
        ESP_LOGI(TAG, "dialling, APN \"%s\"%s", use,
                 use == network_apn && network_apn[0] ? " (the network's)" : "");
        ret = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
    }
    a7682e_lifecycle_finish_operation();
    return ret;
}

static void power_off(void)
{
    if (!a7682e_lifecycle_begin_operation()) {
        return;
    }
    a7682e_power_off_locked();
    a7682e_lifecycle_finish_operation();
    bool powered = false;
    info_update(apply_powered, &powered);
}

/* -------------------------------------------------------------------------
 * hal_net_driver_t
 * ---------------------------------------------------------------------- */

static esp_err_t net_connect(const char *apn, const char *credential, uint32_t timeout_ms)
{
    (void)credential;
    if (a7682e_ppp_up()) {
        atomic_store(&s_state, HAL_NET_STATE_CONNECTED);
        return ESP_OK;
    }
    if (timeout_ms == 0) {
        timeout_ms = CONNECT_DEFAULT_TIMEOUT_MS;
    }
    TickType_t start = xTaskGetTickCount();
    atomic_store(&s_abort, false);
    atomic_store(&s_state, HAL_NET_STATE_CONNECTING);

    esp_err_t ret = power_on();
    bool powered = ret == ESP_OK;
    info_update(apply_powered, &powered);
    if (ret != ESP_OK) {
        goto fail;
    }

    /* An SMS exchange is redialling the session it interrupted. Tearing that
     * down to start over would cost a minute. A session that simply dropped
     * also leaves the DCE in data mode, but is not waited for. */
    if (resuming()) {
        TickType_t waited = xTaskGetTickCount();
        while (!a7682e_ppp_up() && !deadline_passed(waited, RESUME_WAIT_MS)
               && sleep_unless_aborted(250)) {
        }
        if (a7682e_ppp_up()) {
            atomic_store(&s_state, HAL_NET_STATE_CONNECTED);
            return ESP_OK;
        }
    }
    leave_dead_session();

    /* The SIM. A module just started reads it for a few seconds, and says
     * "SIM busy" or even that there is none meanwhile; a verdict other than
     * ready is taken only once it has held for SIM_SETTLE_POLLS answers. */
    int silent = 0;
    int same = 0;
    hal_sim_state_t sim = HAL_SIM_UNKNOWN;
    hal_sim_state_t prev = HAL_SIM_UNKNOWN;
    for (;;) {
        bool answered = false;
        sim = read_sim(&answered);
        info_update(apply_sim, &sim);
        same = sim == prev ? same + 1 : 1;
        prev = sim;
        if (sim == HAL_SIM_READY || (sim != HAL_SIM_UNKNOWN && same >= SIM_SETTLE_POLLS)) {
            break;
        }
        silent = answered ? 0 : silent + 1;
        if (silent >= MAX_SILENT_POLLS) {
            ESP_LOGW(TAG, "modem stopped answering; powering it off");
            power_off();
            ret = ESP_ERR_TIMEOUT;
            goto fail;
        }
        if (deadline_passed(start, timeout_ms) || !sleep_unless_aborted(SIM_POLL_MS)) {
            ret = ESP_ERR_TIMEOUT;
            goto fail;
        }
    }
    if (sim != HAL_SIM_READY) {
        ESP_LOGW(TAG, "SIM not usable (state %d)", (int)sim);
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    /* A registration. Searching can take a minute after a cold start; a
     * denial can lift as the module tries other cells, so it is waited out
     * too. */
    hal_cell_info_t seen;
    hal_cell_reg_t last_reg = HAL_CELL_REG_UNKNOWN;
    silent = 0;
    for (;;) {
        bool answered = read_network(&seen);
        info_update(apply_network, &seen);
        if (seen.reg != last_reg) {
            ESP_LOGI(TAG, "registration %d, %d dBm", (int)seen.reg, (int)seen.rssi_dbm);
            last_reg = seen.reg;
        }
        if (a7682e_reg_is_registered(seen.reg)) {
            break;
        }
        silent = answered ? 0 : silent + 1;
        if (silent >= MAX_SILENT_POLLS) {
            ESP_LOGW(TAG, "modem stopped answering; powering it off");
            power_off();
            ret = ESP_ERR_TIMEOUT;
            goto fail;
        }
        if (deadline_passed(start, timeout_ms) || !sleep_unless_aborted(REG_POLL_MS)) {
            ESP_LOGW(TAG, "no registration");
            ret = ESP_ERR_TIMEOUT;
            goto fail;
        }
    }
    ESP_LOGI(TAG, "registered on %s, %d dBm",
             seen.operator_name[0] ? seen.operator_name : "(unnamed network)",
             (int)seen.rssi_dbm);

    /* The data session. */
    if (atomic_load(&s_abort)) {
        ret = ESP_ERR_TIMEOUT;
        goto fail;
    }
    ret = dial(apn);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "dial failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    TickType_t dialled = xTaskGetTickCount();
    while (!a7682e_ppp_up()) {
        if (deadline_passed(dialled, PPP_TIMEOUT_MS) || !sleep_unless_aborted(250)) {
            ESP_LOGW(TAG, "no address from the network");
            leave_dead_session();
            ret = ESP_ERR_TIMEOUT;
            goto fail;
        }
    }
    atomic_store(&s_state, HAL_NET_STATE_CONNECTED);
    atomic_store(&s_had_session, true);
    ESP_LOGI(TAG, "connected, IP %s", a7682e_ppp_ip());
    return ESP_OK;

fail:
    atomic_store(&s_state, HAL_NET_STATE_DISCONNECTED);
    return ret;
}

/* Every wait in net_connect() checks this, so the attempt ends within one
 * poll, a step under way first: powering the module up, or one AT exchange. */
static void net_cancel(void)
{
    atomic_store(&s_abort, true);
}

static esp_err_t net_disconnect(void)
{
    atomic_store(&s_abort, true);
    atomic_store(&s_had_session, false);
    power_off();
    atomic_store(&s_state, HAL_NET_STATE_DISCONNECTED);
    return ESP_OK;
}

static hal_net_state_t net_get_state(void)
{
    int state = atomic_load(&s_state);
    if (state == HAL_NET_STATE_CONNECTED && !a7682e_ppp_up()) {
        /* The network or the module ended the session. */
        int expected = HAL_NET_STATE_CONNECTED;
        atomic_compare_exchange_strong(&s_state, &expected, HAL_NET_STATE_DISCONNECTED);
        return HAL_NET_STATE_DISCONNECTED;
    }
    return (hal_net_state_t)state;
}

static bool net_is_connected(void)
{
    return net_get_state() == HAL_NET_STATE_CONNECTED;
}

static const char *net_get_ip(void)
{
    return net_is_connected() ? a7682e_ppp_ip() : NULL;
}

static int8_t net_get_rssi(void)
{
    taskENTER_CRITICAL(&s_info_lock);
    int16_t rssi = s_info.rssi_dbm;
    taskEXIT_CRITICAL(&s_info_lock);
    return (int8_t)(rssi < -128 ? -128 : rssi);
}

static esp_err_t net_get_cell_info(hal_cell_info_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_info_lock);
    *out = s_info;
    taskEXIT_CRITICAL(&s_info_lock);
    return ESP_OK;
}

/* The data session ends for sleep; connect() starts it again after. */
static esp_err_t net_sleep(bool enter)
{
    if (!a7682e_lifecycle_begin_operation()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = a7682e_sleep_locked(enter);
    a7682e_lifecycle_finish_operation();
    if (enter && ret == ESP_OK) {
        atomic_store(&s_had_session, false);
        atomic_store(&s_state, HAL_NET_STATE_DISCONNECTED);
    }
    return ret;
}

static const hal_net_driver_t s_net_driver = {
    .type          = HAL_NET_CELLULAR,
    .name          = "LTE",
    .init          = NULL,
    .connect       = net_connect,
    .disconnect    = net_disconnect,
    .get_state     = net_get_state,
    .get_ip        = net_get_ip,
    .get_rssi      = net_get_rssi,
    .is_connected  = net_is_connected,
    .get_cell_info = net_get_cell_info,
    .cancel        = net_cancel,
    .sleep         = net_sleep,
};

const hal_net_driver_t *drv_a7682e_net_driver(void)
{
    return &s_net_driver;
}
