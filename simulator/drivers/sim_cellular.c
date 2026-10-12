/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS Simulator — a cellular modem
 *
 * A hal_net_driver_t of type HAL_NET_CELLULAR standing in for the A7682E on
 * the boards that carry one, so Settings' mobile data page and the network
 * coordinator can be driven without hardware. It carries no traffic: the
 * host network does that. --sim-card picks what the "SIM" does.
 */

#include "hal/network.h"
#include "thistle/net_manager.h"
#include "esp_err.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static hal_sim_state_t s_card = HAL_SIM_READY;
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

static esp_err_t cell_connect(const char *apn, const char *credential, uint32_t timeout_ms)
{
    (void)credential;
    (void)timeout_ms;
    atomic_store(&s_abort, false);
    atomic_store(&s_state, HAL_NET_STATE_CONNECTING);
    printf("[sim_cellular] connecting, APN \"%s\"\n", apn ? apn : "");

    memset(&s_info, 0, sizeof(s_info));
    s_info.powered = true;
    if (!wait_unless_aborted(2)) goto fail;
    s_info.sim = s_card;
    if (s_card != HAL_SIM_READY) {
        atomic_store(&s_state, HAL_NET_STATE_DISCONNECTED);
        return ESP_ERR_NOT_FOUND;
    }

    s_info.reg = HAL_CELL_REG_SEARCHING;
    s_info.rssi_dbm = -97;
    if (!wait_unless_aborted(2)) goto fail;
    s_info.reg = HAL_CELL_REG_HOME;
    s_info.rssi_dbm = -79;
    snprintf(s_info.operator_name, sizeof(s_info.operator_name), "Thistle Mobile");
    if (!wait_unless_aborted(1)) goto fail;

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
};

esp_err_t sim_cellular_register(void)
{
    return net_manager_register(&s_sim_cellular);
}
