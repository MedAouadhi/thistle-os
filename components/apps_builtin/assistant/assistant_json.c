// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors

#include "assistant_json.h"

#include <stdint.h>
#include <string.h>

/* Deep enough for any API response; shallow enough that a hostile document
 * cannot run the recursion off the end of a task stack. */
#define JSON_MAX_DEPTH 32

/* ------------------------------------------------------------------ */
/* Writing                                                              */
/* ------------------------------------------------------------------ */

static bool writer_reserve(assistant_json_writer_t *writer, size_t additional)
{
    if (!writer->valid || additional > SIZE_MAX - writer->length - 1U) {
        writer->valid = false;
        return false;
    }
    if (writer->buffer && writer->length + additional >= writer->capacity) {
        writer->valid = false;
        return false;
    }
    return true;
}

static bool writer_append_bytes(assistant_json_writer_t *writer,
                                const char *bytes, size_t length)
{
    if (!writer_reserve(writer, length)) {
        return false;
    }
    if (writer->buffer && length > 0) {
        memcpy(writer->buffer + writer->length, bytes, length);
    }
    writer->length += length;
    return true;
}

static bool writer_append_char(assistant_json_writer_t *writer, char value)
{
    return writer_append_bytes(writer, &value, 1U);
}

bool assistant_json_write_raw(assistant_json_writer_t *writer, const char *text)
{
    return text && writer_append_bytes(writer, text, strlen(text));
}

bool assistant_json_write_string(assistant_json_writer_t *writer,
                                 const char *value)
{
    static const char hex[] = "0123456789abcdef";
    if (!value || !writer_append_char(writer, '"')) {
        return false;
    }

    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0'; cursor++) {
        const char *escape = NULL;
        switch (*cursor) {
            case '"': escape = "\\\""; break;
            case '\\': escape = "\\\\"; break;
            case '\b': escape = "\\b"; break;
            case '\f': escape = "\\f"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            default: break;
        }
        if (escape) {
            if (!writer_append_bytes(writer, escape, 2U)) {
                return false;
            }
        } else if (*cursor < 0x20U) {
            char unicode_escape[6] = {
                '\\', 'u', '0', '0', hex[*cursor >> 4], hex[*cursor & 0x0f]
            };
            if (!writer_append_bytes(writer, unicode_escape,
                                     sizeof(unicode_escape))) {
                return false;
            }
        } else if (!writer_append_char(writer, (char)*cursor)) {
            return false;
        }
    }
    return writer_append_char(writer, '"');
}

/* ------------------------------------------------------------------ */
/* Reading                                                              */
/* ------------------------------------------------------------------ */

static const char *skip_ws(const char *p, const char *end)
{
    while (p && p < end
           && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    return p;
}

/* p is at an opening quote; returns the byte after the closing one. */
static const char *skip_string(const char *p, const char *end)
{
    if (!p || p >= end || *p != '"') {
        return NULL;
    }
    for (p++; p < end; p++) {
        if (*p == '\\') {
            if (++p >= end) {
                return NULL;
            }
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

static bool is_literal_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z') || c == '-' || c == '+' || c == '.';
}

static const char *skip_value(const char *p, const char *end, int depth)
{
    p = skip_ws(p, end);
    if (!p || p >= end) {
        return NULL;
    }
    if (*p == '"') {
        return skip_string(p, end);
    }
    if (*p == '{' || *p == '[') {
        if (depth >= JSON_MAX_DEPTH) {
            return NULL;
        }
        const bool object = *p == '{';
        const char close = object ? '}' : ']';
        p = skip_ws(p + 1, end);
        if (p < end && *p == close) {
            return p + 1;
        }
        for (;;) {
            if (object) {
                p = skip_ws(skip_string(p, end), end);
                if (!p || p >= end || *p != ':') {
                    return NULL;
                }
                p++;
            }
            p = skip_ws(skip_value(p, end, depth + 1), end);
            if (!p || p >= end) {
                return NULL;
            }
            if (*p == close) {
                return p + 1;
            }
            if (*p != ',') {
                return NULL;
            }
            p = skip_ws(p + 1, end);
        }
    }
    /* A number, true, false or null. */
    const char *start = p;
    while (p < end && is_literal_char(*p)) {
        p++;
    }
    return p > start ? p : NULL;
}

/* The member named segment of the object at p, or element segment of the
 * array at p. */
static const char *find_child(const char *p, const char *end,
                              const char *segment, size_t segment_len)
{
    if (!p || p >= end || segment_len == 0) {
        return NULL;
    }
    if (*p == '{') {
        p = skip_ws(p + 1, end);
        while (p < end && *p == '"') {
            const char *after_key = skip_string(p, end);
            if (!after_key) {
                return NULL;
            }
            const bool match = (size_t)(after_key - p - 2) == segment_len
                && memcmp(p + 1, segment, segment_len) == 0;
            p = skip_ws(after_key, end);
            if (p >= end || *p != ':') {
                return NULL;
            }
            p = skip_ws(p + 1, end);
            if (match) {
                return p < end ? p : NULL;
            }
            p = skip_ws(skip_value(p, end, 1), end);
            if (!p || p >= end || *p != ',') {
                return NULL;
            }
            p = skip_ws(p + 1, end);
        }
        return NULL;
    }
    if (*p == '[') {
        size_t index = 0;
        if (segment_len > 6) {
            return NULL;
        }
        for (size_t i = 0; i < segment_len; i++) {
            if (segment[i] < '0' || segment[i] > '9') {
                return NULL;
            }
            index = index * 10U + (size_t)(segment[i] - '0');
        }
        p = skip_ws(p + 1, end);
        if (p >= end || *p == ']') {
            return NULL;
        }
        for (size_t i = 0; i < index; i++) {
            p = skip_ws(skip_value(p, end, 1), end);
            if (!p || p >= end || *p != ',') {
                return NULL;
            }
            p = skip_ws(p + 1, end);
        }
        return p < end ? p : NULL;
    }
    return NULL;
}

const char *assistant_json_find(const char *json, size_t len, const char *path)
{
    if (!json || !path) {
        return NULL;
    }
    const char *end = json + len;
    const char *value = skip_ws(json, end);
    while (*path != '\0') {
        const char *dot = strchr(path, '.');
        const size_t segment_len = dot ? (size_t)(dot - path) : strlen(path);
        value = find_child(value, end, path, segment_len);
        if (!value) {
            return NULL;
        }
        path += segment_len;
        if (*path == '.') {
            path++;
        }
    }
    return value < end ? value : NULL;
}

static bool read_hex4(const char *p, const char *end, uint32_t *out)
{
    if (end - p < 4) {
        return false;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        const char c = p[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') {
            digit = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = (uint32_t)(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    *out = value;
    return true;
}

static size_t utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80U) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800U) {
        out[0] = (char)(0xC0U | (cp >> 6));
        out[1] = (char)(0x80U | (cp & 0x3FU));
        return 2;
    }
    if (cp < 0x10000U) {
        out[0] = (char)(0xE0U | (cp >> 12));
        out[1] = (char)(0x80U | ((cp >> 6) & 0x3FU));
        out[2] = (char)(0x80U | (cp & 0x3FU));
        return 3;
    }
    out[0] = (char)(0xF0U | (cp >> 18));
    out[1] = (char)(0x80U | ((cp >> 12) & 0x3FU));
    out[2] = (char)(0x80U | ((cp >> 6) & 0x3FU));
    out[3] = (char)(0x80U | (cp & 0x3FU));
    return 4;
}

/* p is just past a backslash-u. Decodes it, and the low half that must
 * follow a high surrogate, advancing *p past what it used. */
static bool decode_unicode_escape(const char **p, const char *end,
                                  uint32_t *cp)
{
    if (!read_hex4(*p, end, cp)) {
        return false;
    }
    *p += 4;
    if (*cp >= 0xD800U && *cp <= 0xDBFFU) {
        uint32_t low;
        if (end - *p >= 6 && (*p)[0] == '\\' && (*p)[1] == 'u'
            && read_hex4(*p + 2, end, &low)
            && low >= 0xDC00U && low <= 0xDFFFU) {
            *cp = 0x10000U + ((*cp - 0xD800U) << 10) + (low - 0xDC00U);
            *p += 6;
        } else {
            *cp = 0xFFFDU;
        }
    } else if (*cp >= 0xDC00U && *cp <= 0xDFFFU) {
        *cp = 0xFFFDU;
    }
    return true;
}

bool assistant_json_get_string(const char *json, size_t len, const char *path,
                               char *out, size_t out_len, bool *truncated)
{
    if (truncated) {
        *truncated = false;
    }
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    const char *p = assistant_json_find(json, len, path);
    const char *end = json + len;
    if (!p || *p != '"') {
        return false;
    }
    p++;

    size_t used = 0;
    bool cut = false;
    while (p < end && *p != '"') {
        char encoded[4];
        size_t encoded_len;
        if (*p == '\\') {
            if (++p >= end) {
                return false;
            }
            const char escape = *p++;
            switch (escape) {
                case '"': case '\\': case '/':
                    encoded[0] = escape; encoded_len = 1; break;
                case 'b': encoded[0] = '\b'; encoded_len = 1; break;
                case 'f': encoded[0] = '\f'; encoded_len = 1; break;
                case 'n': encoded[0] = '\n'; encoded_len = 1; break;
                case 'r': encoded[0] = '\r'; encoded_len = 1; break;
                case 't': encoded[0] = '\t'; encoded_len = 1; break;
                case 'u': {
                    uint32_t cp;
                    if (!decode_unicode_escape(&p, end, &cp)) {
                        return false;
                    }
                    /* A NUL would end the C string early; drop it. */
                    encoded_len = cp == 0 ? 0 : utf8_encode(cp, encoded);
                    break;
                }
                default:
                    return false;
            }
        } else {
            const unsigned char lead = (unsigned char)*p;
            if (lead < 0x20U) {
                return false;
            }
            /* Copy a multi-byte character whole, counting only real
             * continuation bytes so a stray lead byte cannot swallow the
             * closing quote. */
            size_t want = lead >= 0xF0U ? 4 : lead >= 0xE0U ? 3
                        : lead >= 0xC0U ? 2 : 1;
            encoded_len = 1;
            while (encoded_len < want && p + encoded_len < end
                   && ((unsigned char)p[encoded_len] & 0xC0U) == 0x80U) {
                encoded_len++;
            }
            memcpy(encoded, p, encoded_len);
            p += encoded_len;
        }

        if (cut) {
            continue; /* still walk to the closing quote to validate it */
        }
        if (used + encoded_len >= out_len) {
            cut = true;
            continue;
        }
        memcpy(out + used, encoded, encoded_len);
        used += encoded_len;
    }
    if (p >= end) {
        out[0] = '\0';
        return false;
    }
    out[used] = '\0';
    if (truncated) {
        *truncated = cut;
    }
    return true;
}

bool assistant_json_string_equals(const char *json, size_t len,
                                  const char *path, const char *expected)
{
    char value[32];
    bool truncated = false;
    return expected
        && assistant_json_get_string(json, len, path, value, sizeof(value),
                                     &truncated)
        && !truncated && strcmp(value, expected) == 0;
}
