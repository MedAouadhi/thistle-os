/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS Simulator — a cellular modem
 *
 * A hal_net_driver_t of type HAL_NET_CELLULAR standing in for the A7682E on
 * the boards that carry one, so Settings' mobile data page and the network
 * coordinator can be driven without hardware. It carries no traffic: the
 * host network does that. --sim-card picks what the "SIM" does, and --sms
 * puts messages in its storage for the kernel to fetch.
 */

#include "hal/network.h"
#include "thistle/net_manager.h"
#include "esp_err.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static hal_sim_state_t s_card = HAL_SIM_READY;

/* SMS in the "modem's" storage, from --sms, until the kernel keeps them. */
#define SIM_MAX_SMS 8
static hal_sms_t s_sms[SIM_MAX_SMS];
static int s_sms_count;
static atomic_int  s_state = HAL_NET_STATE_DISCONNECTED;
static atomic_bool s_abort;
static hal_cell_info_t s_info;

/* "ready" (the default), "absent", "pin" or "puk". */
void sim_cellular_set_card(const char *card)
{
    if (strcmp(card, "absent") == 0)   s_card = HAL_SIM_ABSENT;
    else if (strcmp(card, "pin") == 0) s_card = HAL_SIM_PIN_REQUIRED;
    else if (strcmp(card, "puk") == 0) s_card = HAL_SIM_PUK_REQUIRED;
    else                               s_card = HAL_SIM_READY;
}

/* A step of a few seconds, as the modem takes, cut short by disconnect. */
static bool wait_unless_aborted(int seconds)
{
    for (int i = 0; i < seconds * 4; i++) {
        if (atomic_load(&s_abort)) return false;
        usleep(250 * 1000);
    }
    return !atomic_load(&s_abort);
}

/* "SENDER: TEXT", held in storage from the start. */
bool sim_cellular_add_sms(const char *spec)
{
    const char *colon = strchr(spec, ':');
    if (!colon || s_sms_count >= SIM_MAX_SMS) return false;
    hal_sms_t *m = &s_sms[s_sms_count++];
    memset(m, 0, sizeof(*m));
    snprintf(m->sender, sizeof(m->sender), "%.*s", (int)(colon - spec), spec);
    const char *text = colon + 1;
    while (*text == ' ') text++;
    snprintf(m->text, sizeof(m->text), "%s", text);
    snprintf(m->sent_at, sizeof(m->sent_at), "25/10/13,12:34:56+08");
    return true;
}

/* Power, the SIM and a registration, a few seconds each as on a modem. */
static esp_err_t attach(void)
{
    if (s_info.powered && s_info.sim == HAL_SIM_READY && s_info.reg == HAL_CELL_REG_HOME) {
        return ESP_OK;
    }
    memset(&s_info, 0, sizeof(s_info));
    s_info.powered = true;
    if (!wait_unless_aborted(2)) return ESP_ERR_TIMEOUT;
    s_info.sim = s_card;
    if (s_card != HAL_SIM_READY) return ESP_ERR_NOT_FOUND;

    s_info.reg = HAL_CELL_REG_SEARCHING;
    s_info.rssi_dbm = -97;
    if (!wait_unless_aborted(2)) return ESP_ERR_TIMEOUT;
    s_info.reg = HAL_CELL_REG_HOME;
    s_info.rssi_dbm = -79;
    snprintf(s_info.operator_name, sizeof(s_info.operator_name), "Thistle Mobile");
    return ESP_OK;
}

static esp_err_t cell_listen(uint32_t timeout_ms)
{
    (void)timeout_ms;
    atomic_store(&s_abort, false);
    printf("[sim_cellular] registering\n");
    esp_err_t ret = attach();
    if (ret == ESP_OK) printf("[sim_cellular] registered\n");
    return ret;
}

static esp_err_t cell_connect(const char *apn, const char *credential, uint32_t timeout_ms)
{
    (void)credential;
    (void)timeout_ms;
    atomic_store(&s_abort, false);
    atomic_store(&s_state, HAL_NET_STATE_CONNECTING);
    printf("[sim_cellular] connecting, APN \"%s\"\n", apn ? apn : "");

    esp_err_t ret = attach();
    if (ret == ESP_ERR_NOT_FOUND) {
        atomic_store(&s_state, HAL_NET_STATE_DISCONNECTED);
        return ret;
    }
    if (ret != ESP_OK || !wait_unless_aborted(1)) goto fail;

    atomic_store(&s_state, HAL_NET_STATE_CONNECTED);
    printf("[sim_cellular] connected\n");
    return ESP_OK;

fail:
    atomic_store(&s_state, HAL_NET_STATE_DISCONNECTED);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t cell_disconnect(void)
{
    atomic_store(&s_abort, true);
    atomic_store(&s_state, HAL_NET_STATE_DISCONNECTED);
    memset(&s_info, 0, sizeof(s_info));
    printf("[sim_cellular] modem off\n");
    return ESP_OK;
}

static hal_net_state_t cell_get_state(void)
{
    return (hal_net_state_t)atomic_load(&s_state);
}

static bool cell_is_connected(void)
{
    return atomic_load(&s_state) == HAL_NET_STATE_CONNECTED;
}

static const char *cell_get_ip(void)
{
    return cell_is_connected() ? "10.64.0.2" : NULL;
}

static int8_t cell_get_rssi(void)
{
    return (int8_t)s_info.rssi_dbm;
}

static esp_err_t cell_get_info(hal_cell_info_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = s_info;
    return ESP_OK;
}

static bool cell_ready(void)
{
    return s_info.powered && s_info.sim == HAL_SIM_READY;
}

static esp_err_t cell_sms_send(const char *number, const char *text)
{
    if (!cell_ready()) return ESP_ERR_INVALID_STATE;
    printf("[sim_cellular] SMS to %s: %s\n", number, text);
    return ESP_OK;
}

static bool cell_sms_due(void)
{
    return cell_ready() && s_sms_count > 0;
}

static esp_err_t cell_sms_fetch(hal_sms_rx_cb_t cb, void *user_data, int *kept)
{
    *kept = 0;
    if (!cell_ready()) return ESP_ERR_INVALID_STATE;
    int left = 0;
    for (int i = 0; i < s_sms_count; i++) {
        if (cb(&s_sms[i], user_data)) {
            (*kept)++;
        } else {
            s_sms[left++] = s_sms[i];
        }
    }
    s_sms_count = left;
    printf("[sim_cellular] SMS fetched: %d\n", *kept);
    return ESP_OK;
}

static const hal_net_driver_t s_sim_cellular = {
    .type          = HAL_NET_CELLULAR,
    .name          = "LTE",
    .connect       = cell_connect,
    .disconnect    = cell_disconnect,
    .get_state     = cell_get_state,
    .get_ip        = cell_get_ip,
    .get_rssi      = cell_get_rssi,
    .is_connected  = cell_is_connected,
    .get_cell_info = cell_get_info,
    .listen        = cell_listen,
    .sms_send      = cell_sms_send,
    .sms_due       = cell_sms_due,
    .sms_fetch     = cell_sms_fetch,
};

esp_err_t sim_cellular_register(void)
{
    return net_manager_register(&s_sim_cellular);
}
