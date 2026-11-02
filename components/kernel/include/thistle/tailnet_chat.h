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
 * A contact's messages are sent through an outbox: delivered at once if it
 * is connected, else dialled for, again on a backoff of 30 s doubling to 30
 * minutes, until acked. Delivered and read receipts, typing, replies and
 * reactions go both ways. A message received buzzes, pops up and counts as
 * unread (tailnet_chat_unread_count(), the status bar's envelope and Home's
 * row) until read, unless its conversation is on screen
 * (tailnet_chat_set_viewing()).
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
    bool     read_receipts;    /* reading sends read receipts */
    uint32_t unread;           /* messages from contacts not yet read */
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
    bool   pending;                  /* added by address, never reached: key is a placeholder */
    uint16_t unread;                 /* its messages not yet read */
    uint16_t waiting;                /* in the outbox for it */
    bool   typing;                   /* it says it is typing */
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
    char     state[12];      /* "received" or "read"; for one sent, "waiting", "delivered", "read" */
    char     reply_to[33];   /* the id it answers, or "" */
    char     react_me[33];   /* this device's reaction, an emoji, or "" */
    char     react_them[33]; /* the contact's */
    uint32_t voice_ms;       /* a voice message's length, or 0 for a text */
    uint32_t voice_bytes;    /* its file's size */
} tailnet_chat_message_t;

uint32_t  tailnet_chat_generation(void);
esp_err_t tailnet_chat_get_status(tailnet_chat_status_t *out);

int       tailnet_chat_contact_count(void);
esp_err_t tailnet_chat_get_contact(int index, tailnet_chat_contact_t *out);

int       tailnet_chat_request_count(void);
esp_err_t tailnet_chat_get_request(int index, tailnet_chat_request_t *out);

/* A contact's latest messages (at most 20), oldest first; none for a
 * stranger. The text is copied into text, cut to text_len - 1 bytes; a
 * voice message's text says what it is ("Voice message, 0:02"). */
int       tailnet_chat_message_count(const char *key);
esp_err_t tailnet_chat_get_message(const char *key, int index, tailnet_chat_message_t *out,
                                   char *text, size_t text_len);

/* The text of message id of contact key, if it is among the 20 kept:
 * what a reply quotes. ESP_ERR_NOT_FOUND otherwise. */
esp_err_t tailnet_chat_find_message(const char *key, const char *id, bool *mine,
                                    char *text, size_t text_len);

/* Message `index` held by the contact request from key, oldest first. */
esp_err_t tailnet_chat_get_held(const char *key, int index, char *text, size_t text_len);

/* Send text (1 to 4096 bytes) to contact key, as a reply to reply_to if it
 * is not NULL or "": shown at once as waiting, delivered through the
 * outbox. Its id into id_out (33 bytes) if not NULL. */
esp_err_t tailnet_chat_send_text(const char *key, const char *text, const char *reply_to, char *id_out);

/* Send a voice message to contact key: `data` is its Ogg/Opus file (at most
 * 512 KB), `ms` long (at most 120000). The module keeps the file beside the
 * history, so the caller may free data once this returns; it goes through
 * the outbox like a text. Its id into id_out (33 bytes) if not NULL. */
esp_err_t tailnet_chat_send_voice(const char *key, const uint8_t *data, size_t len, uint32_t ms,
                                  const char *reply_to, char *id_out);

/* Where voice message `id`'s Ogg/Opus file is, sent or received, into
 * `path` (path_len bytes). ESP_ERR_NOT_FOUND if there is no such file: a
 * voice message is shown only once its file is whole. */
esp_err_t tailnet_chat_voice_path(const char *id, char *path, size_t path_len);

/* React to message `message` of contact key with emoji (at most 32 bytes),
 * "" to remove this device's reaction; delivered like a message. */
esp_err_t tailnet_chat_react(const char *key, const char *message, const char *emoji);

/* The user read what contact key sent: a read receipt goes, if allowed. */
esp_err_t tailnet_chat_mark_read(const char *key);

/* The user is typing to contact key, or stopped: sent if it is connected,
 * never queued. Repeat true every 4 s while typing goes on; it lapses
 * after 6 s at the other end. */
esp_err_t tailnet_chat_set_typing(const char *key, bool on);
bool      tailnet_chat_is_typing(const char *key);

/* The conversation on screen, NULL for none: its messages are read as they
 * arrive and alert nothing. */
void      tailnet_chat_set_viewing(const char *key);

/* Whether reading sends read receipts (thistle_os.tailnet_chat.read_receipts). */
bool      tailnet_chat_read_receipts(void);
esp_err_t tailnet_chat_set_read_receipts(bool on);

/* How often, in minutes, the background check "tailnet chat" dials the
 * contacts while the device sleeps (thistle_os.tailnet_chat.check_minutes,
 * 15); 0 is never. */
uint32_t  tailnet_chat_check_minutes(void);
esp_err_t tailnet_chat_set_check_minutes(uint32_t minutes);

/* Messages received from contacts and not yet read. */
uint32_t  tailnet_chat_unread_count(void);

/* Call a contact over `ws-opus`; the system call screen follows the call
 * through thistle/call.h, as it does a phone call. One call at a time:
 * ESP_ERR_INVALID_STATE with a call up (tailnet or cellular) or chat not
 * listening; ESP_ERR_NOT_FOUND for a key that is not a contact. A contact
 * that cannot be reached ends the call at once as unreachable. */
esp_err_t tailnet_chat_call(const char *key);

/* Add a contact by its tailnet name or address and port (0: 4810); its key
 * is pinned when it is first reached. The key it goes by until then into
 * key_out (65 bytes) if not NULL. */
esp_err_t tailnet_chat_add_contact(const char *host, uint16_t port, const char *name, char *key_out);

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
