/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the SMS app's phone numbers (sms_core.h)
 */
#include "sms/sms_core.h"

#include <string.h>

bool sms_number(const char *typed, char *out, size_t n)
{
    if (!out || n == 0) return false;
    out[0] = '\0';
    if (!typed) return false;
    size_t at = 0;
    bool digits = false;
    for (const char *c = typed; *c; c++) {
        if (*c == ' ' || *c == '-' || *c == '.' || *c == '/' || *c == '(' || *c == ')') continue;
        bool plus = *c == '+' && at == 0;
        if (!plus && (*c < '0' || *c > '9')) {
            out[0] = '\0';
            return false;
        }
        if (at + 1 >= n) {
            out[0] = '\0';
            return false;
        }
        digits = digits || !plus;
        out[at++] = *c;
    }
    out[at] = '\0';
    if (!digits) out[0] = '\0';
    return digits;
}

/* The digits of a number, all else dropped; how many. */
static size_t digits_of(const char *number, char *out, size_t size)
{
    size_t n = 0;
    for (; *number && n < size - 1; number++) {
        if (*number >= '0' && *number <= '9') out[n++] = *number;
    }
    out[n] = '\0';
    return n;
}

bool sms_numbers_match(const char *a, const char *b)
{
    if (!a || !b) return false;
    char ca[40], cb[40];
    if (!sms_number(a, ca, sizeof ca) || !sms_number(b, cb, sizeof cb)) return strcmp(a, b) == 0;
    char da[40], db[40];
    size_t na = digits_of(ca, da, sizeof da);
    size_t nb = digits_of(cb, db, sizeof db);
    if (na >= 9 && nb >= 9) return strcmp(da + na - 9, db + nb - 9) == 0;
    return strcmp(da, db) == 0;
}
