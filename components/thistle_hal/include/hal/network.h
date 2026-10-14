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

/* An SMS a cellular transport received */
typedef struct {
    char     sender[33];   /* "+4917612345678", or a name such as "DHL" */
    /* The service centre's time stamp, "yy/MM/dd,hh:mm:ss+zz" with zz in
     * quarters of an hour, or "" if the modem gave none */
    char     sent_at[24];
    char     text[481];    /* UTF-8: 160 GSM characters take up to 480 bytes */
    /* Part `part` of `parts` of a long message, counted from 1, which the
     * sender numbered `concat_ref`; parts is 0 for a message that is whole */
    uint16_t concat_ref;
    uint8_t  part;
    uint8_t  parts;
} hal_sms_t;

/* Return true once the message is kept: only then is it deleted from the
 * modem. Called with the driver held: it must not call the driver. */
typedef bool (*hal_sms_rx_cb_t)(const hal_sms_t *sms, void *user_data);

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
    /* Cellular only, optional, for the power manager. enter: end any data
     * session and let the modem sleep where it still hears calls and SMS,
     * and raises RI for them. ESP_OK if it now does; ESP_ERR_INVALID_STATE if
     * the modem is off and there is nothing to listen with. !enter: wake it
     * to answer commands; connect() then starts data again. */
    esp_err_t (*sleep)(bool enter);
    /* Cellular only, optional: power the modem and register with a network
     * without a data session, so that SMS come and go. Returns as connect()
     * does: ESP_ERR_NOT_FOUND for a SIM that is missing or locked. connect()
     * starts data from there; disconnect() powers the modem down. */
    esp_err_t (*listen)(uint32_t timeout_ms);
    /* Cellular only, optional: send an SMS to `number` ('+' and digits, or
     * digits), of up to 160 plain ASCII characters. */
    esp_err_t (*sms_send)(const char *number, const char *text);
    /* Cellular only, optional: whether the modem may hold SMS not yet
     * fetched: it signalled one, or it is time to look again. */
    bool (*sms_due)(void);
    /* Cellular only, optional: hand each SMS the modem holds to cb, and
     * delete each one cb kept; *kept says how many. A data session is
     * interrupted for the length of the exchange. */
    esp_err_t (*sms_fetch)(hal_sms_rx_cb_t cb, void *user_data, int *kept);
} hal_net_driver_t;
