/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Network manager
 *
 * Unified network API used by all apps and subsystems.
 * Replaces direct wifi_manager calls — works over WiFi, 4G, BLE tether,
 * the simulator's host network transport, or an overlay VPN that rides on
 * top of one of those underlay transports.
 *
 * Usage:
 *   net_is_connected()         — any transport connected?
 *   net_get_transport_name()   — "WiFi", "4G LTE", "Host Network", ...
 *   net_get_rssi()             — signal strength from active transport
 */
#pragma once

#include "esp_err.h"
#include "hal/network.h"
#include <stdbool.h>
#include <stddef.h>

/* Initialize the network manager (call once during kernel_init) */
esp_err_t net_manager_init(void);

/* Register a network transport (called by WiFi init, 4G init, sim init, etc.) */
esp_err_t net_manager_register(const hal_net_driver_t *driver);

/* Check if ANY registered transport is currently connected */
bool net_is_connected(void);

/* Get the currently active/connected transport (NULL if none) */
const hal_net_driver_t *net_get_active(void);

/* Get the active connected non-VPN underlay transport (NULL if none) */
const hal_net_driver_t *net_get_active_underlay(void);

/* Check if a non-VPN underlay transport is connected */
bool net_has_underlay_connection(void);

/* Get state of the best available connection */
hal_net_state_t net_get_state(void);

/* Get IP address from whichever transport is connected (NULL if none) */
const char *net_get_ip(void);

/* Get signal strength from active transport (0 if none) */
int8_t net_get_rssi(void);

/* Get human-readable name of active transport ("WiFi", "4G LTE", "None", ...) */
const char *net_get_transport_name(void);

/* Try to connect using the transports in order of preference.
 * Returns ESP_OK immediately if already connected. */
esp_err_t net_connect_best(uint32_t timeout_ms);

/* List all registered transports.
 * Returns the number of transports copied into out[] (up to max). */
int net_list_transports(const hal_net_driver_t **out, int max);

/* Register the built-in WiFi transport (wraps wifi_manager).
 * Called once during kernel_init after net_manager_init(). */
esp_err_t net_manager_register_wifi(void);

/* ------------------------------------------------------------------ */
/* Mobile data (components/kernel_rs/src/net_coordinator.rs)          */
/*                                                                    */
/* Traffic takes WiFi whenever it is connected and the cellular link  */
/* otherwise: esp_netif routes by interface priority, and DNS follows */
/* the route. With mobile data on, the link is kept up even while     */
/* WiFi is, so it carries traffic the moment WiFi drops.              */
/* ------------------------------------------------------------------ */

typedef struct {
    bool            present;      /* the board has a cellular modem */
    bool            enabled;      /* mobile data switched on */
    hal_net_state_t data;         /* the data session */
    bool            sim_blocked;  /* the SIM was missing or locked; nothing is
                                     tried until mobile data or SMS is
                                     switched off or on */
    hal_cell_info_t info;         /* what the modem last reported */
    bool            sms;          /* SMS reception switched on */
} net_cellular_status_t;

/* Whether the board has a cellular modem. */
bool net_cellular_present(void);

/* Switch mobile data on or off, saved as thistle_os.cellular.enabled. Returns
 * at once; the modem starts or stops in the background. The choice applies
 * even when saving it fails, which the return reports. */
esp_err_t net_cellular_set_enabled(bool enabled);

/* Switch SMS reception on or off, saved as thistle_os.cellular.sms (on
 * unless switched off). With mobile data off it decides whether the modem
 * is powered at all: registered with a network, without a data session. */
esp_err_t net_cellular_set_sms(bool enabled);

esp_err_t net_cellular_get_status(net_cellular_status_t *out);

/* The APN to dial, saved as thistle_os.cellular.apn: letters, digits, '.',
 * '-' and '_', at most 63 characters. "" or NULL leaves it to the network.
 * A modem already connected is restarted to use a new one. */
esp_err_t net_cellular_set_apn(const char *apn);
esp_err_t net_cellular_get_apn(char *buf, size_t len);
