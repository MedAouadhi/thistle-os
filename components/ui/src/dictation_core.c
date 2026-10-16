/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — dictation's rules that need no hardware.
 */
#include "ui/dictation_core.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Text out                                                             */
/* ------------------------------------------------------------------ */

/* A NUL-terminated string being written into `buf`. Once something does not
 * fit, nothing more is written, so what is there ends between characters. */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool full;
} out_t;

static void out_init(out_t *o, char *buf, size_t cap)
{
    o->buf = buf;
    o->cap = cap;
    o->len = 0;
    o->full = cap == 0;
    if (cap) buf[0] = '\0';
}

/* Appends `n` bytes as one unit: all of them, or none and the end. */
static void out_bytes(out_t *o, const char *bytes, size_t n)
{
    if (o->full) return;
    if (o->len + n >= o->cap) {
        o->full = true;
        return;
    }
    memcpy(o->buf + o->len, bytes, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

static void out_codepoint(out_t *o, uint32_t cp)
{
    char b[4];
    size_t n;
    if (cp < 0x80) {
        b[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    out_bytes(o, b, n);
}

/* How many bytes the UTF-8 character starting with `lead` takes; 1 for a
 * byte that starts none, which is then passed on as it is. */
static size_t utf8_length(uint8_t lead)
{
    if (lead >= 0xF0 && lead < 0xF8) return 4;
    if (lead >= 0xE0) return lead < 0xF0 ? 3 : 1;
    if (lead >= 0xC0) return 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/* JSON in                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    const char *end;
} cursor_t;

#define JSON_MAX_DEPTH 32

static void skip_space(cursor_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
        c->p++;
    }
}

static bool hex4(const char *p, const char *end, uint32_t *out)
{
    if (end - p < 4) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char ch = p[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= (uint32_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= (uint32_t)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= (uint32_t)(ch - 'A' + 10);
        else return false;
    }
    *out = v;
    return true;
}

/* Reads the string at the cursor, which is on its opening quote, into `o`
 * (NULL to skip it). False if it is not a well-formed string. */
static bool read_string(cursor_t *c, out_t *o)
{
    if (c->p >= c->end || *c->p != '"') return false;
    c->p++;
    while (c->p < c->end) {
        uint8_t ch = (uint8_t)*c->p;
        if (ch == '"') {
            c->p++;
            return true;
        }
        if (ch != '\\') {
            size_t n = utf8_length(ch);
            if ((size_t)(c->end - c->p) < n) n = (size_t)(c->end - c->p);
            if (o) out_bytes(o, c->p, n);
            c->p += n;
            continue;
        }
        if (c->end - c->p < 2) return false;
        char esc = c->p[1];
        c->p += 2;
        uint32_t cp;
        switch (esc) {
        case '"':  cp = '"'; break;
        case '\\': cp = '\\'; break;
        case '/':  cp = '/'; break;
        case 'b':  cp = '\b'; break;
        case 'f':  cp = '\f'; break;
        case 'n':  cp = '\n'; break;
        case 'r':  cp = '\r'; break;
        case 't':  cp = '\t'; break;
        case 'u':
            if (!hex4(c->p, c->end, &cp)) return false;
            c->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00) {
                /* A high surrogate wants its low one next; alone, it is
                 * no character. */
                uint32_t low;
                if (c->end - c->p >= 6 && c->p[0] == '\\' && c->p[1] == 'u' &&
                    hex4(c->p + 2, c->end, &low) && low >= 0xDC00 && low < 0xE000) {
                    c->p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp < 0xE000) {
                cp = 0xFFFD;
            }
            break;
        default:
            return false;
        }
        if (o) out_codepoint(o, cp);
    }
    return false;
}

static bool skip_value(cursor_t *c, int depth);

/* Skips an object or array, the cursor on its opening bracket. */
static bool skip_container(cursor_t *c, int depth)
{
    if (depth > JSON_MAX_DEPTH) return false;
    char close = *c->p == '{' ? '}' : ']';
    bool object = close == '}';
    c->p++;
    skip_space(c);
    if (c->p < c->end && *c->p == close) {
        c->p++;
        return true;
    }
    while (c->p < c->end) {
        if (object) {
            if (!read_string(c, NULL)) return false;
            skip_space(c);
            if (c->p >= c->end || *c->p != ':') return false;
            c->p++;
        }
        if (!skip_value(c, depth + 1)) return false;
        skip_space(c);
        if (c->p >= c->end) return false;
        if (*c->p == close) {
            c->p++;
            return true;
        }
        if (*c->p != ',') return false;
        c->p++;
        skip_space(c);
    }
    return false;
}

static bool skip_value(cursor_t *c, int depth)
{
    skip_space(c);
    if (c->p >= c->end) return false;
    char ch = *c->p;
    if (ch == '"') return read_string(c, NULL);
    if (ch == '{' || ch == '[') return skip_container(c, depth);
    /* A number, true, false or null: up to whatever ends it. */
    const char *start = c->p;
    while (c->p < c->end && *c->p != ',' && *c->p != '}' && *c->p != ']' && *c->p != ' ' &&
           *c->p != '\t' && *c->p != '\n' && *c->p != '\r') {
        c->p++;
    }
    return c->p > start;
}

/* The top-level member `key` of the object in json[0..len), decoded into
 * `o`. *found false if it is not there or not a string. False if the text is
 * not a JSON object as far as it was read. */
static bool find_string(const char *json, size_t len, const char *key, out_t *o, bool *found)
{
    *found = false;
    if (!json) return false;
    cursor_t c = { json, json + len };
    skip_space(&c);
    if (c.p >= c.end || *c.p != '{') return false;
    c.p++;
    skip_space(&c);
    if (c.p < c.end && *c.p == '}') return true;
    while (c.p < c.end) {
        char name[48];
        out_t name_out;
        out_init(&name_out, name, sizeof(name));
        if (!read_string(&c, &name_out)) return false;
        skip_space(&c);
        if (c.p >= c.end || *c.p != ':') return false;
        c.p++;
        skip_space(&c);
        if (!name_out.full && strcmp(name, key) == 0 && c.p < c.end && *c.p == '"') {
            if (!read_string(&c, o)) return false;
            *found = true;
            return true;
        }
        if (!skip_value(&c, 1)) return false;
        skip_space(&c);
        if (c.p >= c.end) return false;
        if (*c.p == '}') return true;
        if (*c.p != ',') return false;
        c.p++;
        skip_space(&c);
    }
    return false;
}

bool dictation_json_string(const char *json, size_t len, const char *key, char *out, size_t cap)
{
    out_t o;
    out_init(&o, out, cap);
    bool found;
    if (!find_string(json, len, key, &o, &found) || !found) {
        if (cap) out[0] = '\0';
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

static void copy_text(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

void dictation_config_init(dictation_config_t *config, const char *api_key)
{
    copy_text(config->url, sizeof(config->url), DICTATION_DEFAULT_URL);
    copy_text(config->api_key, sizeof(config->api_key), api_key);
    copy_text(config->model, sizeof(config->model), DICTATION_DEFAULT_MODEL);
    copy_text(config->language, sizeof(config->language), DICTATION_DEFAULT_LANGUAGE);
}

/* Replaces `field` with the member `key`, if the member is a string that is
 * not empty and fits whole. A key or URL cut short works no better than none. */
static bool apply_field(const char *json, size_t len, const char *key, char *field, size_t cap)
{
    char value[DICTATION_URL_MAX];
    out_t o;
    out_init(&o, value, cap < sizeof(value) ? cap : sizeof(value));
    bool found;
    if (!find_string(json, len, key, &o, &found)) return false;
    if (found && !o.full && o.len > 0) memcpy(field, value, o.len + 1);
    return true;
}

bool dictation_config_apply(dictation_config_t *config, const char *json, size_t len)
{
    dictation_config_t next = *config;
    bool ok = apply_field(json, len, "url", next.url, sizeof(next.url)) &&
              apply_field(json, len, "api_key", next.api_key, sizeof(next.api_key)) &&
              apply_field(json, len, "model", next.model, sizeof(next.model));
    if (ok) *config = next;
    return ok;
}

/* ------------------------------------------------------------------ */
/* The request                                                          */
/* ------------------------------------------------------------------ */

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void dictation_wav_header(uint8_t out[DICTATION_WAV_HEADER_BYTES], uint32_t sample_rate,
                          uint32_t pcm_bytes)
{
    const uint16_t channels = 1, bits = 16;
    const uint16_t block = (uint16_t)(channels * bits / 8);
    memcpy(out, "RIFF", 4);
    put_le32(out + 4, 36 + pcm_bytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    put_le32(out + 16, 16);              /* the fmt chunk's size */
    put_le16(out + 20, 1);               /* PCM */
    put_le16(out + 22, channels);
    put_le32(out + 24, sample_rate);
    put_le32(out + 28, sample_rate * block);
    put_le16(out + 32, block);
    put_le16(out + 34, bits);
    memcpy(out + 36, "data", 4);
    put_le32(out + 40, pcm_bytes);
}

/* Whether `value` can go between quotes, or on a line, of the framing. */
static bool framing_safe(const char *value, size_t max)
{
    size_t n = strlen(value);
    if (n == 0 || n > max) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (ch < 0x20 || ch == '"' || ch == 0x7F) return false;
    }
    return true;
}

bool dictation_form_build(dictation_form_t *form, const char *boundary, const char *model,
                          const char *language)
{
    if (!form || !boundary || !model || !language) return false;
    if (!framing_safe(boundary, 40) || !framing_safe(model, DICTATION_MODEL_MAX - 1) ||
        !framing_safe(language, DICTATION_LANGUAGE_MAX - 1)) {
        return false;
    }
    int head = snprintf(form->head, sizeof(form->head),
                        "--%s\r\n"
                        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                        "%s\r\n"
                        "--%s\r\n"
                        "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
                        "%s\r\n"
                        "--%s\r\n"
                        "Content-Disposition: form-data; name=\"file\"; filename=\"dictation.wav\"\r\n"
                        "Content-Type: audio/wav\r\n\r\n",
                        boundary, model, boundary, language, boundary);
    int tail = snprintf(form->tail, sizeof(form->tail), "\r\n--%s--\r\n", boundary);
    int type = snprintf(form->content_type, sizeof(form->content_type),
                        "multipart/form-data; boundary=%s", boundary);
    if (head < 0 || (size_t)head >= sizeof(form->head) || tail < 0 ||
        (size_t)tail >= sizeof(form->tail) || type < 0 ||
        (size_t)type >= sizeof(form->content_type)) {
        return false;
    }
    form->head_len = (size_t)head;
    form->tail_len = (size_t)tail;
    return true;
}

void dictation_status_words(int status, const char *message, char *out, size_t cap)
{
    if (status <= 0) {
        snprintf(out, cap, "Could not reach Cohere");
    } else if (status == 401 || status == 403) {
        snprintf(out, cap, "Cohere refused the API key");
    } else if (status == 429) {
        snprintf(out, cap, "Cohere says too many requests; wait a little");
    } else if (status >= 500) {
        snprintf(out, cap, "Cohere is having trouble; try again");
    } else if (message && message[0]) {
        snprintf(out, cap, "Cohere: %s", message);
    } else {
        snprintf(out, cap, "Cohere answered %d", status);
    }
}

/* ------------------------------------------------------------------ */
/* Into the field                                                       */
/* ------------------------------------------------------------------ */

static bool is_space(uint8_t ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

size_t dictation_join(char *out, size_t cap, const char *transcript, uint8_t before,
                      bool one_line)
{
    out_t o;
    out_init(&o, out, cap);
    if (!transcript) return 0;

    const char *start = transcript;
    const char *end = transcript + strlen(transcript);
    while (start < end && is_space((uint8_t)*start)) start++;
    while (end > start && is_space((uint8_t)end[-1])) end--;
    if (start == end) return 0;

    /* A word before the cursor gets a space after it; a space, a line
     * break or an opening bracket does not, nor does punctuation the
     * transcript opens with. */
    bool after_word = before != 0 && !is_space(before) && before != '(' && before != '[' &&
                      before != '{';
    bool hugs = strchr(".,;:!?)]}", *start) != NULL;
    if (after_word && !hugs) out_bytes(&o, " ", 1);

    for (const char *p = start; p < end && !o.full;) {
        uint8_t ch = (uint8_t)*p;
        if (one_line && (ch == '\r' || ch == '\n' || ch == '\t')) {
            /* "\r\n" is one break, so one space. */
            if (ch == '\r' && p + 1 < end && p[1] == '\n') p++;
            out_bytes(&o, " ", 1);
            p++;
            continue;
        }
        size_t n = utf8_length(ch);
        if ((size_t)(end - p) < n) n = (size_t)(end - p);
        out_bytes(&o, p, n);
        p += n;
    }
    return o.len;
}

uint8_t dictation_byte_before(const char *txt, uint32_t pos)
{
    if (!txt) return 0;
    size_t i = 0;
    for (uint32_t chars = 0; txt[i] && chars < pos; chars++) {
        size_t n = utf8_length((uint8_t)txt[i]);
        while (n-- && txt[i]) i++;
    }
    return i ? (uint8_t)txt[i - 1] : 0;
}
