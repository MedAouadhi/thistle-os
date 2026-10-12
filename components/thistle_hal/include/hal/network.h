/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Network HAL interface
 *
 * Defines the vtable that any network transport (WiFi, 4G/LTE, BLE tether,
 * simulator host network) must implement to plug into the net_manager.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* Network transport types */
typedef enum {
    HAL_NET_WIFI,
    HAL_NET_CELLULAR,   /* 4G/LTE via PPP */
    HAL_NET_BLE,        /* BLE tethering */
    HAL_NET_ETHERNET,   /* Future */
    HAL_NET_HOST,       /* Simulator — uses host OS networking */
    HAL_NET_VPN,        /* Overlay VPN — requires another network transport */
} hal_net_transport_t;

/* Network state */
typedef enum {
    HAL_NET_STATE_DISCONNECTED,
    HAL_NET_STATE_CONNECTING,
    HAL_NET_STATE_CONNECTED,
} hal_net_state_t;

/* The SIM card, as a cellular transport last saw it */
typedef enum {
    HAL_SIM_UNKNOWN,        /* modem off, or not asked yet */
    HAL_SIM_READY,
    HAL_SIM_ABSENT,
    HAL_SIM_PIN_REQUIRED,
    HAL_SIM_PUK_REQUIRED,
    HAL_SIM_ERROR,          /* anything else the modem said about it */
} hal_sim_state_t;

/* Registration with a mobile network */
typedef enum {
    HAL_CELL_REG_UNKNOWN,
    HAL_CELL_REG_NOT_REGISTERED,  /* and not searching */
    HAL_CELL_REG_SEARCHING,
    HAL_CELL_REG_HOME,
    HAL_CELL_REG_ROAMING,
    HAL_CELL_REG_DENIED,
} hal_cell_reg_t;

/* What a cellular transport knows beyond the common state */
typedef struct {
    bool            powered;            /* modem on and answering */
    hal_sim_state_t sim;
    hal_cell_reg_t  reg;
    int16_t         rssi_dbm;           /* 0 if unknown */
    char            operator_name[24];  /* "" if unknown */
} hal_cell_info_t;

/* Network driver vtable */
typedef struct {
    hal_net_transport_t type;
    const char *name;                              /* "WiFi", "4G LTE", "Host" */
    esp_err_t (*init)(void);
    /* WiFi: target = SSID, credential = password.
     * Cellular: target = APN (NULL or "" for the one the network assigns),
     *           credential unused. */
    esp_err_t (*connect)(const char *target, const char *credential, uint32_t timeout_ms);
    esp_err_t (*disconnect)(void);
    hal_net_state_t (*get_state)(void);
    const char *(*get_ip)(void);
    int8_t (*get_rssi)(void);                      /* signal strength, 0 if N/A */
    bool (*is_connected)(void);
    /* Cellular only, optional: SIM, registration and operator. The values
     * are those last read from the modem; a modem carrying data over PPP
     * answers no questions, so they date from before the data session. */
    esp_err_t (*get_cell_info)(hal_cell_info_t *out);
    /* Optional: make a connect() under way on another thread give up soon,
     * without waiting for it. disconnect() still powers the transport down. */
    void (*cancel)(void);
} hal_net_driver_t;
