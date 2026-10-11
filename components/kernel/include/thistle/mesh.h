// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* The MeshCore node the kernel runs from boot (mesh_manager.rs), as built-in
 * apps see it. Mirrors the structs and constants there; loaded apps use the
 * thistle_mesh_* names in thistle_app.h instead. */

#define MESH_TEXT_MAX        160    /* MeshCore's MAX_TEXT_LEN */
#define MESH_PUBLIC_CHANNEL  0      /* the channel every MeshCore node shares */
#define MESH_PATH_UNKNOWN    0xFF   /* no route known: messages to it flood */

/* Advert types, as MeshCore numbers them. */
#define MESH_TYPE_CHAT       1
#define MESH_TYPE_REPEATER   2
#define MESH_TYPE_ROOM       3
#define MESH_TYPE_SENSOR     4

#define MESH_MSG_DIRECT      0
#define MESH_MSG_CHANNEL     1

#define MESH_DELIVERY_SENT       1  /* on air; a channel message gets no further word */
#define MESH_DELIVERY_DELIVERED  2  /* the recipient acknowledged it */
#define MESH_DELIVERY_FAILED     3  /* no acknowledgement after every attempt */

typedef struct {
    uint8_t  pub_key[32];
    uint8_t  name[32];
    uint8_t  name_len;
    uint8_t  node_type;     /* MESH_TYPE_* */
    int8_t   last_rssi;
    uint8_t  path_len;      /* hops, or MESH_PATH_UNKNOWN */
    uint32_t last_seen;     /* its last advert, by its own clock */
    double   lat;
    double   lon;
    bool     has_position;
} mesh_contact_t;

typedef struct {
    uint8_t  sender_key[32];    /* zero for a channel message */
    uint8_t  sender_name[32];
    uint8_t  sender_name_len;
    uint32_t timestamp;         /* by the sender's clock */
    uint8_t  text[200];
    uint16_t text_len;
    uint8_t  kind;              /* MESH_MSG_* */
    uint8_t  channel;
} mesh_message_t;

/* The radio settings the mesh runs on: what another node must be set to. */
typedef struct {
    uint32_t freq_hz;
    uint32_t bw_hz;
    uint8_t  sf;
    uint8_t  cr;            /* 5..8 for 4/5..4/8 */
    int8_t   tx_power_dbm;
} mesh_radio_t;

#ifndef __cplusplus
_Static_assert(sizeof(mesh_radio_t) == 12, "mesh_radio_t must match CMeshRadio");
_Static_assert(sizeof(mesh_contact_t) == 96, "mesh_contact_t must match CMeshContact");
_Static_assert(sizeof(mesh_message_t) == 276, "mesh_message_t must match CMeshMessage");
#endif

/* Whether the node is on air. */
bool rs_mesh_is_running(void);

/* ESP_ERR_INVALID_STATE until the mesh has started. */
esp_err_t rs_mesh_get_radio(mesh_radio_t *out);

/* This node's name (empty until it is running), and its 32-byte key. */
const char *rs_mesh_get_self_name(void);
esp_err_t rs_mesh_get_self_key(uint8_t *out);

/* Rename the node, advertise the new name, and keep it in system.json. */
esp_err_t rs_mesh_set_name(const char *name);

/* Contacts heard from, most recently heard first. */
int rs_mesh_get_contact_count(void);
esp_err_t rs_mesh_get_contact(int index, mesh_contact_t *out);
/* The index of the contact whose key starts with `prefix`, or -1. */
int rs_mesh_find_contact_prefix(const uint8_t *prefix, size_t len);

/* Send up to MESH_TEXT_MAX bytes. `out_id`, if not NULL, gets the id that
 * delivery reports name. ESP_ERR_NOT_FOUND for a key that is not a contact. */
esp_err_t rs_mesh_send_direct(const uint8_t *pub_key, const char *text, uint32_t *out_id);
esp_err_t rs_mesh_send_channel(uint8_t channel, const char *text, uint32_t *out_id);

/* Advertise this node, so peers can add it as a contact. */
esp_err_t rs_mesh_send_advert(void);

/* Take the oldest received message, or the oldest delivery report. Each
 * returns false when there is none waiting. */
bool rs_mesh_inbox_pop(mesh_message_t *out);
bool rs_mesh_delivery_pop(uint32_t *out_id, uint8_t *out_state);

/* Messages received that the user has not read: those still waiting, and
 * those the app that took them reported unread. The app that takes messages
 * reports how many of them it has still to show, again whenever that
 * changes, and 0 when it closes. */
uint32_t rs_mesh_unread_count(void);
void rs_mesh_set_held_unread(uint32_t count);
