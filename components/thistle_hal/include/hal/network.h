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

/* The time a cellular transport last read from its modem's clock, which the
 * mobile network sets (NITZ). Whether the network did is for the reader to
 * judge: a modem's clock that nothing has set counts from its own epoch. */
typedef struct {
    int64_t  utc;             /* seconds since 1970, UTC, as read */
    uint32_t age_ms;          /* how long ago it was read */
    int16_t  utc_offset_min;  /* local time minus UTC, summer time included */
    bool     offset_known;    /* the network gave the offset */
} hal_net_time_t;

/* Return true once the message is kept: only then is it deleted from the
 * modem. Called with the driver held: it must not call the driver. */
typedef bool (*hal_sms_rx_cb_t)(const hal_sms_t *sms, void *user_data);

/* A voice call's state, as the modem lists it: the values are those of
 * <stat> in 3GPP TS 27.007's +CLCC. */
typedef enum {
    HAL_CALL_ACTIVE   = 0,  /* connected */
    HAL_CALL_HELD     = 1,
    HAL_CALL_DIALING  = 2,  /* outgoing, not yet ringing at the far end */
    HAL_CALL_ALERTING = 3,  /* outgoing, ringing at the far end */
    HAL_CALL_INCOMING = 4,  /* ringing here */
    HAL_CALL_WAITING  = 5,  /* incoming while another call is up */
    HAL_CALL_RELEASED = 6,  /* being disconnected */
} hal_call_state_t;

/* One voice call the modem has */
typedef struct {
    uint8_t          id;          /* the modem's index for it, from 1 */
    bool             incoming;    /* mobile terminated */
    hal_call_state_t state;
    char             number[33];  /* "+4917612345678"; "" when withheld */
} hal_call_t;

/* The longest dial string a cellular transport takes, NUL excluded */
#define HAL_CALL_NUMBER_MAX 32

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
    /* Cellular only, optional: the modem's clock as last read, which the
     * driver does while the modem answers commands anyway, never by
     * interrupting a data session. ESP_ERR_NOT_FOUND until it has been read
     * since the modem powered on. Appended last, so no field above moves. */
    esp_err_t (*get_network_time)(hal_net_time_t *out);

    /* Voice calls. Cellular only, optional, all or none; appended after
     * get_network_time so no field above moves.
     *
     * A call needs the modem answering commands, so a data session is left
     * for command mode while any call is up, and resumed by call_list once
     * it finds none: from the first call function until then, connect()
     * must not dial. None of these blocks for longer than one exchange with
     * the modem, plus leaving a data session (a few seconds). */

    /* Place a voice call to `number`: an optional '+', then digits, '*' and
     * '#', up to HAL_CALL_NUMBER_MAX. Returns once the modem has taken the
     * command (ATD); call_list follows the call from there. */
    esp_err_t (*call_dial)(const char *number);
    /* Answer the call ringing here (ATA). */
    esp_err_t (*call_answer)(void);
    /* End every call: hang up a connected or outgoing one, decline one
     * ringing here (AT+CHUP). */
    esp_err_t (*call_hangup)(void);
    /* The modem's calls (AT+CLCC), up to max into calls[], *count how many;
     * 0 when there are none, which is when a data session the calls
     * interrupted is resumed. ESP_ERR_INVALID_STATE while the modem is off
     * or asleep. */
    esp_err_t (*call_list)(hal_call_t *calls, int max, int *count);
    /* Whether a call is ringing here, by the modem's RI line, which it holds
     * low until the call is answered or given up: false for an SMS's short
     * pulse. Reads a pin and never touches the UART or the driver's lock,
     * so it is cheap to ask every second; false without an RI line. */
    bool (*call_ringing)(void);
    /* A DTMF tone in a connected call: one of 0-9 * # A-D (AT+VTS). */
    esp_err_t (*call_dtmf)(char digit);
    /* Mute or unmute the microphone in a connected call (AT+CMUT). */
    esp_err_t (*call_set_mute)(bool mute);
    /* The call's volume, 0-100, for this call and the ones after it
     * (AT+COUTGAIN). */
    esp_err_t (*call_set_volume)(uint8_t percent);
    /* The modem's audio set for a loudspeaker (hands-free) or for an
     * earpiece or headset (AT+CSDVC=3 or 1). */
    esp_err_t (*call_set_speaker)(bool speaker);
} hal_net_driver_t;
