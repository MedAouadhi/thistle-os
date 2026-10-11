// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — MeshCore engine, C interface
//
// The engine runs a MeshCore chat node on its own task, on whatever radio the
// HAL has registered. Its only caller is the kernel's mesh_manager, which owns
// everything apps see: the inbox, the contact list, delivery states. Commands
// are queued to the engine's task; what it hears comes back through the
// callbacks, made on that task.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MESHCORE_NAME_MAX       32
#define MESHCORE_PUBKEY_SIZE    32
#define MESHCORE_TEXT_MAX       160   /* MeshCore's MAX_TEXT_LEN */
#define MESHCORE_PATH_MAX       48

/* Advert types, as MeshCore numbers them. */
#define MESHCORE_TYPE_CHAT      1
#define MESHCORE_TYPE_REPEATER  2
#define MESHCORE_TYPE_ROOM      3
#define MESHCORE_TYPE_SENSOR    4

/* A path length MeshCore uses for "no known route: flood". */
#define MESHCORE_PATH_UNKNOWN   0xFF

#define MESHCORE_MSG_DIRECT     0
#define MESHCORE_MSG_CHANNEL    1

/* Delivery states reported for a message the kernel asked the engine to send. */
#define MESHCORE_SENT           1   /* on air; a channel message gets no further word */
#define MESHCORE_DELIVERED      2   /* the recipient acknowledged it */
#define MESHCORE_FAILED         3   /* no acknowledgement after every attempt */

typedef struct {
    uint32_t freq_hz;
    uint32_t bw_hz;
    uint8_t  sf;
    uint8_t  cr;            /* 5..8 for 4/5..4/8; the radio's own, used for airtime */
    int8_t   tx_power_dbm;
    char     name[MESHCORE_NAME_MAX];        /* empty: derived from the key */
    char     state_dir[MESHCORE_PATH_MAX];   /* where identity and contacts live */
} meshcore_config_t;

typedef struct {
    uint8_t  pub_key[MESHCORE_PUBKEY_SIZE];
    char     name[MESHCORE_NAME_MAX];
    uint8_t  type;          /* MESHCORE_TYPE_* */
    uint8_t  path_len;      /* hops to it, or MESHCORE_PATH_UNKNOWN */
    uint32_t last_advert;   /* by the contact's clock */
    int32_t  lat_e6;        /* 0, 0 when it advertised no position */
    int32_t  lon_e6;
} meshcore_contact_t;

typedef struct {
    uint8_t  kind;          /* MESHCORE_MSG_* */
    uint8_t  channel;       /* channel index, for a channel message */
    uint8_t  sender_key[MESHCORE_PUBKEY_SIZE];   /* zero for a channel message */
    char     sender_name[MESHCORE_NAME_MAX];
    uint32_t timestamp;     /* by the sender's clock */
    char     text[MESHCORE_TEXT_MAX + 1];
} meshcore_message_t;

typedef struct {
    /* Once, when the engine is on air: who this node is. */
    void (*on_ready)(const uint8_t *pub_key, const char *name);
    /* A contact learnt from an advert, loaded from storage, or updated. */
    void (*on_contact)(const meshcore_contact_t *contact);
    /* A contact dropped to make room for a newer one. */
    void (*on_contact_removed)(const uint8_t *pub_key);
    void (*on_message)(const meshcore_message_t *message);
    void (*on_delivery)(uint32_t message_id, uint8_t state);
} meshcore_callbacks_t;

typedef struct {
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t messages_sent;
    uint32_t messages_received;
    uint32_t rx_dropped;    /* packets the radio heard that the engine had no room for */
} meshcore_stats_t;

/* Bring the node up on the registered radio. ESP_ERR_NOT_FOUND with no radio,
 * ESP_ERR_INVALID_STATE if already running. The callbacks must outlive it. */
esp_err_t meshcore_start(const meshcore_config_t *config, const meshcore_callbacks_t *callbacks);

bool meshcore_running(void);

/* Queue a direct message to a known contact. Its delivery is reported against
 * `message_id`. ESP_ERR_INVALID_SIZE for text over MESHCORE_TEXT_MAX. */
esp_err_t meshcore_send_direct(const uint8_t *pub_key, const char *text, uint32_t message_id);

/* Queue a message to a group channel; channel 0 is the Public channel. */
esp_err_t meshcore_send_channel(uint8_t channel, const char *text, uint32_t message_id);

/* Queue an advert, flooded, so nodes beyond direct range learn of this one. */
esp_err_t meshcore_send_advert(void);

/* Rename this node, and advertise the new name. */
esp_err_t meshcore_set_name(const char *name);

esp_err_t meshcore_get_stats(meshcore_stats_t *out);

#ifdef __cplusplus
}
#endif
