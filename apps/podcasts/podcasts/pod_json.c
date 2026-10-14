/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — a JSON reader that walks the text in place.
 */
#include "podcasts/pod_json.h"

#include <string.h>

void pod_json_init(pod_json_t *j, const char *text, size_t len)
{
    j->p = text;
    j->end = text + len;
    j->error = text == NULL;
}

static void ws(pod_json_t *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) {
        j->p++;
    }
}

/* The next value's first character, or 0 at the end or after an error. */
static char peek(pod_json_t *j)
{
    if (j->error) return 0;
    ws(j);
    return j->p < j->end ? *j->p : 0;
}

static bool fail(pod_json_t *j)
{
    j->error = true;
    return false;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool read_hex4(pod_json_t *j, uint32_t *out)
{
    if (j->end - j->p < 4) return fail(j);
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int d = hex_digit(j->p[i]);
        if (d < 0) return fail(j);
        v = v << 4 | (uint32_t)d;
    }
    j->p += 4;
    *out = v;
    return true;
}

/* Appends `cp` as UTF-8 if all of it fits, leaving room for the NUL; once
 * one character does not fit, nothing after it is added either. */
static void put_cp(char *out, size_t cap, size_t *len, bool *full, uint32_t cp)
{
    if (!out || *full) return;
    char buf[4];
    size_t n;
    if (cp < 0x80) {
        buf[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | cp >> 6);
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | cp >> 12);
        buf[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        buf[0] = (char)(0xF0 | cp >> 18);
        buf[1] = (char)(0x80 | (cp >> 12 & 0x3F));
        buf[2] = (char)(0x80 | (cp >> 6 & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*len + n + 1 > cap) {
        *full = true;
        return;
    }
    memcpy(out + *len, buf, n);
    *len += n;
}

/* Reads the string at the cursor, its opening quote included, into `out`
 * (NULL to only pass it). Raw bytes are copied as they are, a multi-byte
 * character whole or not at all. */
static bool read_string(pod_json_t *j, char *out, size_t cap)
{
    size_t len = 0;
    bool full = out == NULL || cap == 0;
    if (out && cap) out[0] = '\0';
    if (j->p >= j->end || *j->p != '"') return fail(j);
    j->p++;

    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p;
        if (c == '"') {
            j->p++;
            if (out && cap) out[len] = '\0';
            return true;
        }
        if (c == '\\') {
            if (j->end - j->p < 2) return fail(j);
            char e = j->p[1];
            j->p += 2;
            uint32_t cp;
            switch (e) {
            case '"':  cp = '"'; break;
            case '\\': cp = '\\'; break;
            case '/':  cp = '/'; break;
            case 'b':  cp = '\b'; break;
            case 'f':  cp = '\f'; break;
            case 'n':  cp = '\n'; break;
            case 'r':  cp = '\r'; break;
            case 't':  cp = '\t'; break;
            case 'u':
                if (!read_hex4(j, &cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* A high surrogate; its low half follows as \uDCxx. */
                    uint32_t lo;
                    if (j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                        j->p += 2;
                        if (!read_hex4(j, &lo)) return false;
                        cp = lo >= 0xDC00 && lo <= 0xDFFF
                                 ? 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
                                 : 0xFFFD;
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                if (cp == 0) cp = 0xFFFD;  /* a NUL would end the string early */
                break;
            default:
                return fail(j);
            }
            put_cp(out, cap, &len, &full, cp);
            continue;
        }

        /* A raw byte: a character's first byte says how many follow. */
        size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
                 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if ((size_t)(j->end - j->p) < n) n = (size_t)(j->end - j->p);
        if (!full) {
            if (len + n + 1 > cap) {
                full = true;
            } else {
                memcpy(out + len, j->p, n);
                len += n;
            }
        }
        j->p += n;
    }
    return fail(j);
}

/* Passes a number, true, false or null, copying its text into `out`. */
static bool read_bare(pod_json_t *j, char *out, size_t cap)
{
    const char *start = j->p;
    while (j->p < j->end) {
        char c = *j->p;
        if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
        j->p++;
    }
    if (j->p == start) return fail(j);
    if (out && cap) {
        size_t n = (size_t)(j->p - start);
        if (n > cap - 1) n = cap - 1;
        memcpy(out, start, n);
        out[n] = '\0';
    }
    return true;
}

void pod_json_skip(pod_json_t *j)
{
    char c = peek(j);
    if (c == '"') {
        read_string(j, NULL, 0);
        return;
    }
    if (c != '{' && c != '[') {
        if (c) read_bare(j, NULL, 0);
        else fail(j);
        return;
    }

    /* An object or an array: count brackets, minding strings. */
    int depth = 0;
    while (j->p < j->end) {
        c = *j->p;
        if (c == '"') {
            if (!read_string(j, NULL, 0)) return;
            continue;
        }
        j->p++;
        if (c == '{' || c == '[') {
            depth++;
        } else if (c == '}' || c == ']') {
            if (--depth == 0) return;
        }
    }
    fail(j);
}

bool pod_json_object(pod_json_t *j)
{
    if (peek(j) != '{') {
        pod_json_skip(j);
        return false;
    }
    j->p++;
    return true;
}

bool pod_json_array(pod_json_t *j)
{
    if (peek(j) != '[') {
        pod_json_skip(j);
        return false;
    }
    j->p++;
    return true;
}

bool pod_json_key(pod_json_t *j, char *key, size_t key_cap)
{
    char c = peek(j);
    if (c == '}') {
        j->p++;
        return false;
    }
    if (c == ',') {
        j->p++;
        c = peek(j);
    }
    if (c != '"') return fail(j);
    if (!read_string(j, key, key_cap)) return false;
    if (peek(j) != ':') return fail(j);
    j->p++;
    return true;
}

bool pod_json_next(pod_json_t *j)
{
    char c = peek(j);
    if (c == ']') {
        j->p++;
        return false;
    }
    if (c == ',') {
        j->p++;
        c = peek(j);
    }
    if (!c || c == ']' || c == '}') return fail(j);
    return true;
}

bool pod_json_null(pod_json_t *j)
{
    if (peek(j) == 'n' && j->end - j->p >= 4 && memcmp(j->p, "null", 4) == 0) {
        j->p += 4;
        return true;
    }
    return false;
}

bool pod_json_string(pod_json_t *j, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    char c = peek(j);
    if (c == '"') return read_string(j, out, cap);
    if (pod_json_null(j)) return true;
    if (c == '{' || c == '[' || !c) {
        pod_json_skip(j);
        return false;
    }
    return read_bare(j, out, cap);
}

bool pod_json_int(pod_json_t *j, int64_t *out)
{
    *out = 0;
    char text[32];
    char c = peek(j);
    if (c == '"') {
        if (!read_string(j, text, sizeof text)) return false;
    } else if (c == '-' || (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n') {
        if (!read_bare(j, text, sizeof text)) return false;
    } else {
        pod_json_skip(j);
        return false;
    }

    if (strcmp(text, "true") == 0) {
        *out = 1;
        return true;
    }
    if (strcmp(text, "false") == 0 || strcmp(text, "null") == 0) return true;

    const char *s = text;
    bool neg = *s == '-';
    if (neg) s++;
    if (*s < '0' || *s > '9') return false;
    int64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        if (v > (INT64_MAX - 9) / 10) return false;
        v = v * 10 + (*s++ - '0');
    }
    *out = neg ? -v : v;
    return true;
}
