/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — File Transfer: the web server.
 *
 * Serves a web page on port 80 that lists, uploads, downloads, edits,
 * renames and removes files on the SD card and on SPIFFS, to whoever signs
 * in with the password the device shows (xfer_auth.h). It answers as
 * XFER_HOSTNAME.local over mDNS, and only on WiFi: a connection that came in
 * on any other network, mobile data above all, is closed unanswered.
 *
 * The app starts it when it opens and stops it when it closes. Meanwhile it
 * keeps the device awake, since sleep stops WiFi, and stops itself once
 * nobody has used it for XFER_IDLE_STOP_S. It waits for WiFi when there is
 * none, and comes back with the same password when WiFi does.
 *
 * Requests are answered one at a time, on the server's own task; every
 * function here may be called from any other task.
 */
#pragma once

#include "filexfer/xfer_auth.h"

#include <stdbool.h>
#include <stdint.h>

#define XFER_HOSTNAME     "thistledev"   /* answers as thistledev.local */
#define XFER_IDLE_STOP_S  (15 * 60)

typedef enum {
    XFER_OFF,       /* stopped: by the user, or unused for XFER_IDLE_STOP_S */
    XFER_NO_WIFI,   /* to run once WiFi is up */
    XFER_RUNNING,
    XFER_FAILED,    /* would not start; `error` says why */
} xfer_state_t;

typedef struct {
    xfer_state_t state;
    char         password[XFER_PASSWORD_LEN + 1];   /* "" while off */
    char         ip[16];
    uint16_t     port;
    bool         mdns;              /* XFER_HOSTNAME.local answers */
    int          sessions;          /* browsers signed in */
    uint32_t     failed_logins;     /* wrong passwords, since this one */
    bool         locked;            /* sign-ins refused for now */
    bool         idle_stopped;      /* off because nobody used it */
    char         activity[64];      /* the last thing done: "Saved todo.txt" */
    char         error[48];
} xfer_status_t;

/* Switches the server on with a new password. It starts at once if WiFi is
 * up, else as soon as it is. */
void xfer_server_start(void);

/* Switches it off, forgetting the password and who signed in. */
void xfer_server_stop(void);

/* A new password, signing out everyone signed in with the old one. */
void xfer_server_new_password(void);

/* Starts and stops the server as WiFi comes and goes, and switches it off
 * once nobody has used it for XFER_IDLE_STOP_S. The app calls it every
 * second. */
void xfer_server_poll(void);

/* Everything the screen shows, zeroed first, so that two may be compared
 * with memcmp(). */
void xfer_server_status(xfer_status_t *out);
