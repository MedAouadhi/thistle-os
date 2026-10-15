/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the tailnet (Tailscale)
 *
 * While the user has it on, the device joins their tailnet over whichever
 * network carries traffic, WiFi or mobile data. Once it is connected any
 * socket reaches a peer's 100.x.y.z address through the tunnel, and
 * getaddrinfo() resolves the tailnet's names: "homelab", or
 * "homelab.tail1234.ts.net". Traffic for the internet does not go through
 * the tailnet.
 *
 * Joining needs nothing compiled in. A device that has not joined before
 * signs in at the URL in auth_url, which the Tailscale app shows as a QR
 * code. After that the device keeps its own identity and rejoins by itself.
 *
 * Only the S3 boards have the client; elsewhere tailnet_available() is
 * false. Settings live in system.json under thistle_os.tailnet. Implemented
 * in components/kernel_rs/src/tailnet_manager.rs, decisions in
 * tailnet_policy.rs, the client behind tailnet_platform.c.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TAILNET_OFF,             /* switched off */
    TAILNET_NO_NETWORK,      /* on, waiting for WiFi or mobile data */
    TAILNET_NO_CLOCK,        /* on, waiting for the clock to be set */
    TAILNET_CONNECTING,
    TAILNET_NEEDS_LOGIN,     /* sign in at auth_url */
    TAILNET_NEEDS_APPROVAL,  /* signed in; an admin of the tailnet must
                                approve the device */
    TAILNET_CONNECTED,       /* on the tailnet, as ip */
    TAILNET_FAILED,          /* stopped, for the reason in error */
    TAILNET_UNAVAILABLE,     /* no client on this board */
} tailnet_state_t;

typedef struct {
    tailnet_state_t state;
    bool     enabled;
    bool     joined;          /* this device has joined a tailnet before */
    char     ip[16];          /* "100.101.102.103" while connected, else "" */
    char     hostname[64];    /* the device's name on the tailnet */
    char     auth_url[192];   /* while TAILNET_NEEDS_LOGIN, else "" */
    char     error[128];      /* why it stopped, else "" */
    uint16_t peers;
} tailnet_status_t;

typedef struct {
    char ip[16];
    char name[64];            /* "homelab.tail1234.ts.net" */
    bool direct;              /* reached directly rather than by a relay */
} tailnet_peer_t;

/* Whether this board has the client. */
bool tailnet_available(void);

/* Switch the tailnet on or off, saved as thistle_os.tailnet.enabled. Returns
 * at once; the client starts or stops in the background. Switching on again
 * starts afresh after a refusal. */
bool tailnet_is_enabled(void);
esp_err_t tailnet_set_enabled(bool on);

bool tailnet_is_connected(void);
esp_err_t tailnet_get_status(tailnet_status_t *out);

/* The peers, while connected. */
int tailnet_peer_count(void);
esp_err_t tailnet_get_peer(int index, tailnet_peer_t *out);

/* The device's name on the tailnet, saved as thistle_os.tailnet.hostname:
 * made lower case, letters, digits and '-'. "" or NULL for the default,
 * thistle-xxxxxx from the MAC address. A running client starts again. */
esp_err_t tailnet_set_hostname(const char *name);

/* Sign out: switch the tailnet off and erase this device's identity, so the
 * next time it is switched on it joins afresh. Returns at once. The device
 * stays in the tailnet's admin console until it is removed there. */
esp_err_t tailnet_forget(void);

#ifdef __cplusplus
}
#endif
