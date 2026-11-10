/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the conversations a chat app keeps (the SMS and MeshCore apps),
 * with no LVGL in it (host-tested by apps/chatkit/tests/run_tests.sh).
 *
 * The kernel hands each received message over once, from its inbox, so an
 * app keeps what it was handed for as long as the device runs: the app is
 * destroyed whenever another comes to the front, its store is not. Each
 * conversation keeps its latest CHATKIT_HISTORY messages; older ones stay on
 * the card (thistle/msg_log.h). A store full of conversations makes room for
 * a new one by forgetting the one longest quiet, unread ones last and a
 * sticky one (the mesh's Public channel) or the one on screen never.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHATKIT_CONVS    16
#define CHATKIT_HISTORY  20
#define CHATKIT_TEXT     512   /* bytes of a message kept, cut at a character */
#define CHATKIT_ADDR     40    /* a conversation's address: a number, a key prefix */
#define CHATKIT_NAME     48    /* a name, UTF-8 */

/* What became of a message the user sent. */
typedef enum {
    CHATKIT_NONE = 0,    /* received, or nothing is known */
    CHATKIT_PENDING,     /* handed to the radio or the modem, word to come */
    CHATKIT_SENT,        /* sent; nothing more will be known */
    CHATKIT_DELIVERED,   /* the recipient acknowledged it */
    CHATKIT_FAILED,      /* it did not get through */
} chatkit_status_t;

typedef struct {
    char     text[CHATKIT_TEXT];
    char     who[CHATKIT_NAME];   /* who wrote it, for a group's; "" for the user's */
    uint64_t when;                /* seconds since 1970, 0 if not known */
    uint32_t ref;                 /* the transport's reference for it, 0 if none */
    uint8_t  status;              /* chatkit_status_t */
    bool     mine;
} chatkit_msg_t;

typedef struct {
    char          conv[CHATKIT_ADDR];  /* "" for a transport's own, the Public channel */
    char          title[CHATKIT_NAME]; /* who it is with */
    uint32_t      count;               /* messages ever kept; the slot is the index modulo the history */
    uint16_t      unread;
    bool          sticky;              /* never forgotten for room */
    uint64_t      active;              /* the store's sequence at its latest message or start */
    chatkit_msg_t msgs[CHATKIT_HISTORY];
} chatkit_conv_t;

typedef struct {
    chatkit_conv_t *convs[CHATKIT_CONVS];
    int             n;
    uint64_t        seq;
    uint32_t        generation;      /* changes with anything in the store */
    bool            history_read;    /* the card's history, read back once a boot */
    char            viewing[CHATKIT_ADDR];
    bool            viewing_set;     /* a conversation is on screen: viewing */
    /* How a conversation is allocated: calloc when NULL. The device's
     * apps put them in PSRAM. */
    void *(*alloc)(size_t size);
    void  (*release)(void *p);
} chatkit_store_t;

/* `src` into `dst`, cut at a whole UTF-8 character if it is too long. */
void chatkit_copy(char *dst, size_t size, const char *src);

chatkit_conv_t *chatkit_find(chatkit_store_t *st, const char *conv);

/* The conversation `conv`, started if there is none, NULL if there is no
 * room or memory. A `title` that is not empty names it, a newer one
 * replacing the old. */
chatkit_conv_t *chatkit_ensure(chatkit_store_t *st, const char *conv, const char *title, bool sticky);

/* Keep a message in `c`, its oldest giving way. A received one is unread
 * unless its conversation is on screen. */
chatkit_msg_t *chatkit_add(chatkit_store_t *st, chatkit_conv_t *c, bool mine, const char *who,
                           const char *text, uint64_t when, uint32_t ref, chatkit_status_t status);

/* News of a message sent with reference `ref`: false if none has it. */
bool chatkit_set_status(chatkit_store_t *st, uint32_t ref, chatkit_status_t status);

/* The conversation on screen, NULL for none: its messages arrive read. */
void chatkit_set_viewing(chatkit_store_t *st, const char *conv);

/* Everything in `c` has been seen. */
void chatkit_mark_read(chatkit_store_t *st, chatkit_conv_t *c);

uint32_t chatkit_unread(const chatkit_store_t *st);

/* Name `c` again: its title and the writer of each message received. */
void chatkit_retitle(chatkit_store_t *st, chatkit_conv_t *c, const char *title);

/* Messages `c` keeps, and the `i`th of them, oldest first. */
int chatkit_shown(const chatkit_conv_t *c);
const chatkit_msg_t *chatkit_msg(const chatkit_conv_t *c, int i);
chatkit_msg_t *chatkit_msg_mut(chatkit_conv_t *c, int i);

/* The conversations, the latest active first, into `out`; how many. */
int chatkit_order(const chatkit_store_t *st, chatkit_conv_t **out, int max);

/* Forget everything, as at a restart. */
void chatkit_clear(chatkit_store_t *st);
