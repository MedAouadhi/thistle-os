/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — File Transfer: the password and who has signed in with it.
 *
 * The device shows a password, made afresh each time the server starts; the
 * web page signs in with it and gets a session token back as a cookie.
 * Scripts can send the password with each request instead, as HTTP Basic
 * authentication (curl -u :PASSWORD).
 *
 * After XFER_FREE_TRIES wrong passwords in a row, sign-ins are refused for
 * XFER_LOCK_FIRST_S seconds, then twice as long after each further wrong
 * one, up to XFER_LOCK_MAX_S. A right one ends that. A new password ends it
 * too, and signs everyone out.
 *
 * Nothing here knows the time or where randomness comes from: callers pass
 * them in, so all of it runs in the host tests. Not thread safe; the server
 * holds its lock around every call.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XFER_PASSWORD_CHARS  8     /* 5 bits each: 40 bits */
#define XFER_PASSWORD_LEN    9     /* as shown: "ABCD-EFGH" */
#define XFER_TOKEN_BYTES     16
#define XFER_TOKEN_LEN       (XFER_TOKEN_BYTES * 2)
#define XFER_SESSIONS        4
#define XFER_FREE_TRIES      5
#define XFER_LOCK_FIRST_S    30
#define XFER_LOCK_MAX_S      600

typedef struct {
    char     token[XFER_TOKEN_LEN + 1];   /* "" when the slot is free */
    int64_t  last_ms;
} xfer_session_t;

typedef struct {
    char           password[XFER_PASSWORD_LEN + 1];
    xfer_session_t sessions[XFER_SESSIONS];
    uint32_t       failures;        /* wrong passwords in a row */
    uint32_t       failures_total;  /* since the password was made */
    int64_t        locked_until_ms;
} xfer_auth_t;

typedef enum {
    XFER_LOGIN_OK,
    XFER_LOGIN_WRONG,
    XFER_LOGIN_LOCKED,   /* not even looked at */
} xfer_login_t;

/* A new password from `random`, one byte a character; every session ended
 * and every count cleared. */
void xfer_auth_reset(xfer_auth_t *auth, const uint8_t random[XFER_PASSWORD_CHARS]);

/* Whether `typed` is the password: in either case, with or without the
 * dash, with spaces around it. Counts a wrong one, and refuses to look while
 * sign-ins are refused; `retry_after_s`, if not NULL, gets how long that
 * lasts. Does not start a session. */
xfer_login_t xfer_auth_check(xfer_auth_t *auth, const char *typed, int64_t now_ms,
                             uint32_t *retry_after_s);

/* Starts a session with a token made from `random`, ending the least
 * recently used one if all are taken, and writes the token to `token`. */
void xfer_auth_open_session(xfer_auth_t *auth, const uint8_t random[XFER_TOKEN_BYTES],
                            int64_t now_ms, char token[XFER_TOKEN_LEN + 1]);

/* Whether `token` is a session's, marking it used now if so. */
bool xfer_auth_session(xfer_auth_t *auth, const char *token, int64_t now_ms);

void xfer_auth_close_session(xfer_auth_t *auth, const char *token);

int xfer_auth_session_count(const xfer_auth_t *auth);

/* Whether sign-ins are being refused at `now_ms`. */
bool xfer_auth_locked(const xfer_auth_t *auth, int64_t now_ms);

/* The password in an Authorization header's "Basic <base64(user:password)>",
 * whatever the user. False for any other kind, bad base64, or no ':'. */
bool xfer_basic_password(const char *header, char *out, size_t cap);

/* The value of the cookie `name` in a Cookie header. */
bool xfer_cookie_value(const char *header, const char *name, char *out, size_t cap);
