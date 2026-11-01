/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Tailnet chat
 *
 * This device as a chat peer on the user's tailnet (ADR-0015,
 * docs/tailnet-chat/protocol.md). While the tailnet is connected the kernel
 * listens on its tailnet address, port thistle_os.tailnet_chat.port (4810),
 * whether or not an app is open, and proves the device's MeshCore identity
 * to each chat peer that connects.
 *
 * A chat peer whose key is not a contact's is a stranger: what it sends is
 * held as a contact request until the user accepts it (the key is pinned,
 * the messages appear, the sender is told) or declines it (they are
 * dropped, the sender is told). A chat peer under a contact's name or
 * address with another key is refused and the contact flagged, until the
 * user accepts the new key in its place.
 *
 * Keys are passed as 64 lowercase hex digits. Every call returns at once;
 * tailnet_chat_generation() changes whenever anything shown here does, so a
 * screen redraws only then. Implemented in
 * components/kernel_rs/src/tailnet_chat.rs, decisions in
 * tailnet_chat_policy.rs.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TAILNET_CHAT_UNAVAILABLE,   /* no tailnet client on this board */
    TAILNET_CHAT_TAILNET_OFF,   /* Tailscale is off, or stopped */
    TAILNET_CHAT_NOT_SIGNED_IN, /* Tailscale waits for a sign-in or an admin */
    TAILNET_CHAT_WAITING,       /* Tailscale is connecting, or waits for a network */
    TAILNET_CHAT_NO_IDENTITY,   /* no MeshCore identity on this device yet */
    TAILNET_CHAT_NO_PORT,       /* the port could not be listened on */
    TAILNET_CHAT_LISTENING,
} tailnet_chat_state_t;

typedef struct {
    tailnet_chat_state_t state;
    char     why[96];          /* what to fix, unless listening */
    uint16_t port;             /* listened on, or 0 */
    char     key[65];          /* this device's identity key, once known */
    char     fingerprint[25];  /* "630d cd29 66c4 3366 9112" */
    char     name[64];         /* the name this device shows: its tailnet hostname */
    uint16_t contacts;
    uint16_t requests;
    uint16_t connections;      /* chat peers connected now */
    uint32_t generation;
} tailnet_chat_status_t;

typedef struct {
    char   key[65];
    char   fingerprint[25];
    char   name[65];                 /* what the user calls it */
    char   seen_name[65];            /* the name it last came under */
    bool   flagged;                  /* another key came under its name or address */
    char   flagged_fingerprint[25];  /* that key's */
    bool   connected;
    int8_t accepted_by_them;         /* -1 not known, 0 declined, 1 accepted */
} tailnet_chat_contact_t;

typedef struct {
    char     key[65];
    char     fingerprint[25];
    char     name[65];       /* the name it came under */
    uint16_t held;           /* messages held */
    char     preview[96];    /* the latest of them, on one line */
} tailnet_chat_request_t;

typedef struct {
    char     id[33];
    bool     mine;
    uint64_t ts;             /* when written, ms since 1970, the writer's clock */
    char     state[12];      /* "received"; for one sent, "waiting", "delivered", "read" */
} tailnet_chat_message_t;

uint32_t  tailnet_chat_generation(void);
esp_err_t tailnet_chat_get_status(tailnet_chat_status_t *out);

int       tailnet_chat_contact_count(void);
esp_err_t tailnet_chat_get_contact(int index, tailnet_chat_contact_t *out);

int       tailnet_chat_request_count(void);
esp_err_t tailnet_chat_get_request(int index, tailnet_chat_request_t *out);

/* A contact's latest messages (at most 20), oldest first; none for a
 * stranger. The text is copied into text, cut to text_len - 1 bytes. */
int       tailnet_chat_message_count(const char *key);
esp_err_t tailnet_chat_get_message(const char *key, int index, tailnet_chat_message_t *out,
                                   char *text, size_t text_len);

/* A contact request: ESP_ERR_NOT_FOUND if there is none from key. */
esp_err_t tailnet_chat_accept(const char *key);
esp_err_t tailnet_chat_decline(const char *key);

/* The contact pinned to key: its flagged key in its place
 * (ESP_ERR_INVALID_STATE if none is flagged), a new name, or removed: its
 * next message is a contact request again. */
esp_err_t tailnet_chat_accept_new_key(const char *key);
esp_err_t tailnet_chat_rename(const char *key, const char *name);
esp_err_t tailnet_chat_remove(const char *key);

#ifdef __cplusplus
}
#endif
