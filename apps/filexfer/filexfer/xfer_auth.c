/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — File Transfer: the password and who has signed in with it.
 */
#include "filexfer/xfer_auth.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

/* 32 characters, so a random byte maps to one without bias; no 0, 1, O or I,
 * which look alike on the screen. */
static const char ALPHABET[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

/* Compares all of both, whatever matches first, so the time taken says
 * nothing about how much of a guess was right. */
static bool same(const char *a, const char *b, size_t len)
{
    unsigned char diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

void xfer_auth_reset(xfer_auth_t *auth, const uint8_t random[XFER_PASSWORD_CHARS])
{
    memset(auth, 0, sizeof *auth);
    size_t n = 0;
    for (int i = 0; i < XFER_PASSWORD_CHARS; i++) {
        if (i == XFER_PASSWORD_CHARS / 2) auth->password[n++] = '-';
        auth->password[n++] = ALPHABET[random[i] & 31];
    }
    auth->password[n] = '\0';
}

static uint32_t lock_seconds(uint32_t failures)
{
    uint32_t s = XFER_LOCK_FIRST_S;
    for (uint32_t i = XFER_FREE_TRIES; i < failures && s < XFER_LOCK_MAX_S; i++) s *= 2;
    return s < XFER_LOCK_MAX_S ? s : XFER_LOCK_MAX_S;
}

bool xfer_auth_locked(const xfer_auth_t *auth, int64_t now_ms)
{
    return now_ms < auth->locked_until_ms;
}

/* The typed password as the device shows it, or "" if it cannot be one. */
static void canonical(const char *typed, char out[XFER_PASSWORD_LEN + 1])
{
    char chars[XFER_PASSWORD_CHARS];
    size_t n = 0;
    out[0] = '\0';
    for (const char *p = typed; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '-' || isspace(c)) continue;
        if (n == XFER_PASSWORD_CHARS) return;
        chars[n++] = (char)toupper(c);
    }
    if (n != XFER_PASSWORD_CHARS) return;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (i == XFER_PASSWORD_CHARS / 2) out[o++] = '-';
        out[o++] = chars[i];
    }
    out[o] = '\0';
}

xfer_login_t xfer_auth_check(xfer_auth_t *auth, const char *typed, int64_t now_ms,
                             uint32_t *retry_after_s)
{
    if (retry_after_s) *retry_after_s = 0;
    if (xfer_auth_locked(auth, now_ms)) {
        if (retry_after_s) {
            *retry_after_s = (uint32_t)((auth->locked_until_ms - now_ms + 999) / 1000);
        }
        return XFER_LOGIN_LOCKED;
    }

    char guess[XFER_PASSWORD_LEN + 1];
    canonical(typed ? typed : "", guess);
    if (auth->password[0] && strlen(guess) == XFER_PASSWORD_LEN &&
        same(guess, auth->password, XFER_PASSWORD_LEN)) {
        auth->failures = 0;
        auth->locked_until_ms = 0;
        return XFER_LOGIN_OK;
    }

    auth->failures++;
    auth->failures_total++;
    if (auth->failures >= XFER_FREE_TRIES) {
        uint32_t s = lock_seconds(auth->failures);
        auth->locked_until_ms = now_ms + (int64_t)s * 1000;
        if (retry_after_s) *retry_after_s = s;
    }
    return XFER_LOGIN_WRONG;
}

void xfer_auth_open_session(xfer_auth_t *auth, const uint8_t random[XFER_TOKEN_BYTES],
                            int64_t now_ms, char token[XFER_TOKEN_LEN + 1])
{
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < XFER_TOKEN_BYTES; i++) {
        token[i * 2] = HEX[random[i] >> 4];
        token[i * 2 + 1] = HEX[random[i] & 15];
    }
    token[XFER_TOKEN_LEN] = '\0';

    xfer_session_t *slot = &auth->sessions[0];
    for (int i = 0; i < XFER_SESSIONS; i++) {
        xfer_session_t *s = &auth->sessions[i];
        if (!s->token[0]) {
            slot = s;
            break;
        }
        if (s->last_ms < slot->last_ms) slot = s;
    }
    memcpy(slot->token, token, XFER_TOKEN_LEN + 1);
    slot->last_ms = now_ms;
}

static xfer_session_t *find(xfer_auth_t *auth, const char *token)
{
    if (!token || strlen(token) != XFER_TOKEN_LEN) return NULL;
    for (int i = 0; i < XFER_SESSIONS; i++) {
        xfer_session_t *s = &auth->sessions[i];
        if (s->token[0] && same(s->token, token, XFER_TOKEN_LEN)) return s;
    }
    return NULL;
}

bool xfer_auth_session(xfer_auth_t *auth, const char *token, int64_t now_ms)
{
    xfer_session_t *s = find(auth, token);
    if (!s) return false;
    s->last_ms = now_ms;
    return true;
}

void xfer_auth_close_session(xfer_auth_t *auth, const char *token)
{
    xfer_session_t *s = find(auth, token);
    if (s) memset(s, 0, sizeof *s);
}

int xfer_auth_session_count(const xfer_auth_t *auth)
{
    int n = 0;
    for (int i = 0; i < XFER_SESSIONS; i++) n += auth->sessions[i].token[0] != '\0';
    return n;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool xfer_basic_password(const char *header, char *out, size_t cap)
{
    if (!header || cap == 0) return false;
    while (*header == ' ') header++;
    if (strncasecmp(header, "Basic ", 6) != 0) return false;
    const char *p = header + 6;
    while (*p == ' ') p++;

    char decoded[128];
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *p && *p != '='; p++) {
        int v = b64_value(*p);
        if (v < 0) return false;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n + 1 >= sizeof decoded) return false;
            decoded[n++] = (char)(acc >> bits & 0xFF);
        }
    }
    while (*p == '=') p++;
    while (*p == ' ') p++;
    if (*p) return false;
    decoded[n] = '\0';

    bool ok = false;
    const char *colon = memchr(decoded, '\0', n) ? NULL : strchr(decoded, ':');
    if (colon && strlen(colon + 1) + 1 <= cap) {
        memcpy(out, colon + 1, strlen(colon + 1) + 1);
        ok = true;
    }
    memset(decoded, 0, sizeof decoded);
    return ok;
}

bool xfer_cookie_value(const char *header, const char *name, char *out, size_t cap)
{
    if (!header || !name || cap == 0) return false;
    size_t name_len = strlen(name);
    const char *p = header;
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > name_len && strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            size_t vlen = len - name_len - 1;
            if (vlen + 1 > cap) return false;
            memcpy(out, p + name_len + 1, vlen);
            out[vlen] = '\0';
            return true;
        }
        p += len;
    }
    return false;
}
