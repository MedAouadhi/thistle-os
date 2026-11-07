/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the screens of a chat app, as Chat (apps/tailchat) draws its
 * own, for an app that brings its transport: the SMS and MeshCore apps.
 *
 * Two tabs, Chats and the app's contacts, and the pages they open: a
 * conversation, Settings, and the whole of a message too long for its
 * bubble. Each list is one lv_table drawn in its draw events, as Chat's
 * are, made for e-paper first: a page is drawn again only when what it
 * shows has changed.
 *
 * The app keeps its conversations in a chatkit_store_t of its own, which
 * outlives its screens, and hands the kit what its transport brings from
 * its poll(), with chatkit_receive() and chatkit_sent_status(). Only one
 * chat app is in front at a time, so the kit's screen state is one.
 */
#pragma once

#include "chatkit/chatkit_store.h"

#include "esp_err.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHATKIT_ROWS 34   /* a list's rows */

typedef enum {
    CHATKIT_AVATAR_SOLID,    /* a known contact */
    CHATKIT_AVATAR_HOLLOW,   /* a number or node with no name, an action */
} chatkit_avatar_t;

/* A row of a list. `kind` is the app's own, but for the kit's settings
 * (below CHATKIT_ROW_KIT); `key` says which. */
#define CHATKIT_ROW_KIT    200
#define CHATKIT_ROW_CHAT   200   /* a conversation, in Chats */
#define CHATKIT_ROW_SAVE   201   /* Settings: save messages to the card */
#define CHATKIT_ROW_ALERTS 202   /* Settings: vibrate and pop up */

typedef struct {
    uint8_t  kind;
    uint8_t  avatar;     /* chatkit_avatar_t */
    uint8_t  tick;       /* chatkit_status_t of the latest, if the user wrote it */
    bool     online;
    bool     toggle;     /* a setting's switch, drawn on the right */
    bool     on;
    uint16_t badge;
    char     badge_text[8];
    char     key[CHATKIT_ADDR];
    char     mono[12];   /* the monogram: initials, or a picture */
    char     title[96];
    char     right[24];
    char     detail[200];
} chatkit_row_t;

typedef struct {
    const char *tag;            /* the log's tag: "sms", "meshcore" */
    const char *name;           /* the Chats tab's title: "SMS", "MeshCore" */
    const char *contacts_tab;   /* the second tab's name: "Contacts", "Nodes" */
    const char *chats_icon;     /* the tabs' pictures */
    const char *contacts_icon;
    const char *empty_title;    /* what Chats says with no conversation */
    const char *empty_text;
    const char *search_hint;    /* not NULL: the contacts tab has a search field */
    int         alert_transport;/* MSG_ALERT_SMS or MSG_ALERT_MESH: the kernel's
                                   alerts stay quiet for the conversation on screen */
    chatkit_store_t *store;

    /* As the app opens, and twice a second while it is in front: hand the
     * kit what the transport brought, and take any request to open a
     * conversation. */
    void (*poll)(void);
    /* Once a boot, as the app first opens: the history the card kept
     * (thistle/msg_log.h), through chatkit_keep(). */
    void (*read_history)(void);
    /* NULL when messages can be sent; else why not, shown in a banner. */
    const char *(*why_not)(void);
    /* Changes whenever what the contacts tab lists may have. */
    uint32_t (*contacts_seq)(void);
    int  (*contacts)(chatkit_row_t *rows, int max, const char *query);
    void (*contact_chosen)(const chatkit_row_t *row);
    /* The app's settings, above the kit's own. */
    int  (*settings)(chatkit_row_t *rows, int max);
    void (*setting_chosen)(const chatkit_row_t *row);
    /* The card at the top of Settings: who this device is to the others. */
    void (*own_card)(char *title, size_t title_n, char *text, size_t text_n);
    /* A conversation's monogram (initials if left empty), the line under
     * its name, and whether it is a group, whose messages show their
     * writers. */
    void (*describe)(const chatkit_conv_t *c, char *mono, size_t mono_n, char *subtitle, size_t sub_n,
                     bool *group);
    /* Send `text` to `conv`; on ESP_OK, `ref` (0 for none) names it in
     * later news, and `status` is what became of it so far. */
    esp_err_t (*send)(const char *conv, const char *text, uint32_t *ref, chatkit_status_t *status);
    uint32_t (*text_limit)(const char *conv);
    /* A call to the conversation's peer; NULL for no Call button. */
    esp_err_t (*call)(const char *conv);
    /* How many messages the app holds unread, for the status bar. */
    void (*held_unread)(uint32_t count);
} chatkit_app_t;

esp_err_t chatkit_ui_create(lv_obj_t *parent, const chatkit_app_t *app);
void chatkit_ui_destroy(void);

/* A message received: kept in `conv` (started, named `title`, if new),
 * unread unless on screen. `who` wrote it, `when` in seconds since 1970 or
 * 0 for now. */
void chatkit_receive(const char *conv, const char *title, const char *who, const char *text, uint64_t when);

/* A message from the card's history, kept read. */
void chatkit_keep(const char *conv, const char *title, bool mine, const char *who, const char *text,
                  uint64_t when);

void chatkit_sent_status(uint32_t ref, chatkit_status_t status);

/* Open the conversation with `conv`, started and named `title` if new. */
void chatkit_open(const char *conv, const char *title);

/* A row for a list, its name and detail fitted to the row. */
void chatkit_fill_row(chatkit_row_t *r, uint8_t kind, const char *key, const char *name, const char *right,
                      const char *detail);
/* A picture in the row's monogram's place. */
void chatkit_row_icon(chatkit_row_t *r, const char *icon);
/* A switch on the row's right, and its detail fitted beside it. */
void chatkit_row_toggle(chatkit_row_t *r, bool on, const char *detail);

/* The lists again, from the app's contacts and settings. */
void chatkit_refresh(void);
