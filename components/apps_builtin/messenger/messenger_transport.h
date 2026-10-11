/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Messenger transport abstraction layer
 *
 * Each transport backend (LoRa mesh, SMS, BLE, Internet) implements the
 * msg_transport_driver_t interface and registers itself at init time.
 * The messenger UI keeps conversations per transport and per address within
 * it, and calls send/receive without knowing the underlying hardware.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Transport type enum                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    MSG_TRANSPORT_LORA     = 0,  /* MeshCore over LoRa                */
    MSG_TRANSPORT_SMS      = 1,  /* SMS via 4G modem (A7682E)        */
    MSG_TRANSPORT_BLE      = 2,  /* BLE relay through phone companion */
    MSG_TRANSPORT_INTERNET = 3,  /* HTTP/WebSocket (future)          */
    MSG_TRANSPORT_COUNT
} msg_transport_t;

/* What became of a message the user sent. */
typedef enum {
    MSG_STATUS_NONE = 0,     /* the transport does not say                */
    MSG_STATUS_PENDING,      /* handed over, or queued for a retry        */
    MSG_STATUS_SENT,         /* on air; nothing more will be known        */
    MSG_STATUS_DELIVERED,    /* the recipient acknowledged it             */
    MSG_STATUS_FAILED,       /* it did not get through                    */
} msg_status_t;

/* ------------------------------------------------------------------ */
/* Callbacks                                                            */
/* ------------------------------------------------------------------ */

/*
 * msg_rx_cb_t — a message arrived.
 *
 * transport  — which backend received it
 * conv       — which conversation within the transport it belongs to: ""
 *              for the transport's own (the mesh's Public channel, the SMS
 *              inbox), otherwise the transport's address for it (a mesh
 *              contact's key, in hex)
 * sender     — human-readable sender (e.g. "Heltec V3", "+447700900123")
 * text       — null-terminated message text
 */
typedef void (*msg_rx_cb_t)(msg_transport_t transport,
                            const char *conv,
                            const char *sender,
                            const char *text);

/* msg_status_cb_t — news of a message sent with a reference. */
typedef void (*msg_status_cb_t)(msg_transport_t transport,
                                uint32_t ref,
                                msg_status_t status);

/* ------------------------------------------------------------------ */
/* Transport driver interface                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    msg_transport_t  type;
    const char      *name;          /* Short display name: "MeshCore", "SMS", ...  */
    const char      *icon;          /* ASCII prefix shown in conversation list     */
    const char      *default_conv;  /* What its "" conversation is called          */

    /*
     * is_available() — returns true if the hardware/connection needed by
     * this transport is present and ready.  Called on the LVGL task.
     */
    bool (*is_available)(void);

    /*
     * send() — transmit text to dest.
     *   dest     NULL or "" for the transport's own conversation.
     *   out_ref  if not NULL, gets a reference later status reports name,
     *            or 0 if this transport reports nothing further.
     *   Returns ESP_OK on success, ESP_ERR_NOT_SUPPORTED if unavailable.
     */
    esp_err_t (*send)(const char *dest, const char *text, uint32_t *out_ref);

    /*
     * start_receive() — begin listening; cb will be called for every
     * inbound message, from any task.  Idempotent.
     */
    esp_err_t (*start_receive)(msg_rx_cb_t cb);

    /*
     * stop_receive() — stop listening.  Safe to call when already stopped.
     */
    void (*stop_receive)(void);

    /*
     * poll() — hand over whatever has been waiting: messages to rx, status
     * reports to status. Called on the LVGL task about once a second. NULL
     * for a transport that only calls back.
     */
    void (*poll)(msg_rx_cb_t rx, msg_status_cb_t status);
} msg_transport_driver_t;

/* ------------------------------------------------------------------ */
/* Registry API                                                         */
/* ------------------------------------------------------------------ */

/*
 * messenger_transport_init() — register all built-in transports.
 * Must be called once before any other transport function.
 */
void messenger_transport_init(void);

/*
 * messenger_register_transport() — add a transport to the registry.
 * driver must point to a statically-allocated driver struct.
 */
void messenger_register_transport(const msg_transport_driver_t *driver);

/*
 * messenger_get_transport() — look up a driver by type.
 * Returns NULL if the type has not been registered.
 */
const msg_transport_driver_t *messenger_get_transport(msg_transport_t type);

/*
 * messenger_get_available_transports() — fill out[] with pointers to
 * every registered driver whose is_available() returns true.
 * Returns the number of entries written (≤ max).
 */
int messenger_get_available_transports(const msg_transport_driver_t **out, int max);

/* ------------------------------------------------------------------ */
/* Mesh addressing                                                      */
/* ------------------------------------------------------------------ */

/* A mesh conversation's address: the first MSG_MESH_KEY_PREFIX bytes of the
 * contact's key, in hex, as MeshCore itself shortens keys. */
#define MSG_MESH_KEY_PREFIX  6
#define MSG_MESH_ADDR_LEN    (MSG_MESH_KEY_PREFIX * 2 + 1)

void messenger_mesh_address(const uint8_t *pub_key, char out[MSG_MESH_ADDR_LEN]);
