// SPDX-License-Identifier: BSD-3-Clause
// Reading the SMS the A7682E holds from its AT+CPMS?, AT+CMGD=? and AT+CMGR answers.
#include "a7682e_sms.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static const char *skip_spaces(const char *p)
{
    while (*p == ' ') {
        p++;
    }
    return p;
}

int a7682e_parse_storage_used(const char *resp)
{
    if (!resp) {
        return -1;
    }
    const char *p = strstr(resp, "+CPMS:");
    if (!p) {
        return -1;
    }
    p = skip_spaces(p + strlen("+CPMS:"));
    // AT+CPMS? names each memory before its counts, quoted or not; the write
    // and execution forms start at <used1>.
    if (*p == '"') {
        p = strchr(p + 1, '"');
        if (!p) {
            return -1;
        }
        p = skip_spaces(p + 1);
        if (*p != ',') {
            return -1;
        }
        p++;
    } else if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
        while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            p++;
        }
        p = skip_spaces(p);
        if (*p != ',') {
            return -1;
        }
        p++;
    }
    p = skip_spaces(p);
    if (!is_digit(*p)) {
        return -1;
    }
    char *end = NULL;
    long used = strtol(p, &end, 10);
    if (used > INT_MAX) {
        return -1;
    }
    return (int)used;
}

int a7682e_parse_index_list(const char *resp, int *out, int max)
{
    if (!resp) {
        return -1;
    }
    const char *p = strstr(resp, "+CMGD:");
    if (!p) {
        return -1;
    }
    p = skip_spaces(p + strlen("+CMGD:"));
    if (*p != '(') {
        return -1;
    }
    p++;
    if (!out || max < 0) {
        max = 0;
    }
    int n = 0;
    // Stops at the closing parenthesis, or at anything that is not a list of
    // numbers and ranges, keeping what came before.
    while (n < max) {
        p = skip_spaces(p);
        if (!is_digit(*p)) {
            break;
        }
        char *end = NULL;
        long lo = strtol(p, &end, 10);
        long hi = lo;
        p = skip_spaces(end);
        if (*p == '-') {
            p = skip_spaces(p + 1);
            if (!is_digit(*p)) {
                break;
            }
            hi = strtol(p, &end, 10);
            p = end;
        }
        if (lo < 1) {
            lo = 1;  // storage indices start at one
        }
        if (hi > INT_MAX) {
            hi = INT_MAX;
        }
        // Counted so that a range ending at INT_MAX cannot overflow.
        for (long i = lo; i <= hi && n < max; i++) {
            out[n++] = (int)i;
            if (i == hi) {
                break;
            }
        }
        p = skip_spaces(p);
        if (*p != ',') {
            break;
        }
        p++;
    }
    return n;
}

// --- AT+CMGR ---------------------------------------------------------------

// <stat>,<oa>,<alpha>,<scts>,<tooa>,<fo>,<pid>,<dcs>,<sca>,<tosca>,<length>
#define CMGR_FIELDS   12
#define F_STAT        0
#define F_OA          1
#define F_SCTS        3
#define F_FO          5
#define F_DCS         7
#define F_LENGTH      10

#define FO_MTI_MASK   0x03  // 0 is SMS-DELIVER (TS 23.040 9.2.3.1)
#define FO_UDHI       0x40

typedef struct {
    const char *s;
    size_t len;
    bool quoted;
} field_t;

// Splits a header line at its commas, which a quoted string may also hold.
// Returns how many fields it found, at most `max`, or -1 for a string with
// no closing quote or something other than a comma after one.
static int split_fields(const char *p, const char *end, field_t *f, int max)
{
    int n = 0;
    for (;;) {
        while (p < end && *p == ' ') {
            p++;
        }
        field_t cur = { p, 0, false };
        if (p < end && *p == '"') {
            const char *close = memchr(p + 1, '"', (size_t)(end - p - 1));
            if (!close) {
                return -1;
            }
            cur.s = p + 1;
            cur.len = (size_t)(close - p - 1);
            cur.quoted = true;
            p = close + 1;
            while (p < end && *p == ' ') {
                p++;
            }
            if (p < end && *p != ',') {
                return -1;
            }
        } else {
            const char *q = p;
            while (q < end && *q != ',') {
                q++;
            }
            const char *t = q;
            while (t > p && t[-1] == ' ') {
                t--;
            }
            cur.len = (size_t)(t - p);
            p = q;
        }
        if (n < max) {
            f[n++] = cur;
        }
        if (p >= end) {
            return n;
        }
        p++;  // past the comma
    }
}

static bool field_is(const field_t *f, const char *s)
{
    return f->len == strlen(s) && memcmp(f->s, s, f->len) == 0;
}

static bool field_is_number(const field_t *f)
{
    if (f->len == 0) {
        return false;
    }
    for (size_t i = 0; i < f->len; i++) {
        if (!is_digit(f->s[i])) {
            return false;
        }
    }
    return true;
}

// 1 with *v set, 0 for an empty field, -1 for one that is not a number up to max.
static int field_uint(const field_t *f, unsigned long max, unsigned long *v)
{
    if (f->len == 0) {
        return 0;
    }
    if (!field_is_number(f)) {
        return -1;
    }
    unsigned long n = 0;
    for (size_t i = 0; i < f->len; i++) {
        n = n * 10 + (unsigned long)(f->s[i] - '0');
        if (n > max) {
            return -1;
        }
    }
    *v = n;
    return 1;
}

// --- Hex ---

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static bool is_hex(const char *s, size_t len)
{
    if (len % 2 != 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (hex_val(s[i]) < 0) {
            return false;
        }
    }
    return true;
}

static bool is_ucs2_hex(const char *s, size_t len)
{
    return len > 0 && len % 4 == 0 && is_hex(s, len);
}

// Octet i of a string already checked by is_hex().
static unsigned octet_at(const char *hex, size_t i)
{
    return (unsigned)(hex_val(hex[2 * i]) << 4 | hex_val(hex[2 * i + 1]));
}

static uint32_t unit_at(const char *hex, size_t i)
{
    return (uint32_t)(octet_at(hex, 2 * i) << 8 | octet_at(hex, 2 * i + 1));
}

// --- Output ---

// Writes whole UTF-8 characters into a fixed buffer, keeping it NUL
// terminated. The first character that does not fit ends the string, so it
// is cut at a character boundary and never resumes with a shorter one.
typedef struct {
    char *buf;
    size_t size;
    size_t len;
    bool full;
    bool newlines;  // keep '\n'; every other control character is dropped
} sink_t;

static sink_t sink_for(char *buf, size_t size, bool newlines)
{
    sink_t s = { buf, size, 0, false, newlines };
    buf[0] = '\0';
    return s;
}

static void sink_put(sink_t *s, uint32_t cp)
{
    if (s->full) {
        return;
    }
    // C0 and C1 controls and DEL. CR goes with them, which makes a CR LF
    // line end a plain LF and drops the CR TS 23.038 pads 7-bit text with.
    if ((cp < 0x20 && !(cp == '\n' && s->newlines)) || (cp >= 0x7F && cp < 0xA0)) {
        return;
    }
    char u[4];
    size_t n;
    if (cp < 0x80) {
        u[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | cp >> 6);
        u[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | cp >> 12);
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        u[0] = (char)(0xF0 | cp >> 18);
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (s->len + n >= s->size) {
        s->full = true;
        return;
    }
    memcpy(s->buf + s->len, u, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

// One UTF-8 sequence at b, or 0 if there is none: a stray continuation
// byte, a cut-off or overlong sequence, a surrogate or past U+10FFFF.
static size_t utf8_decode(const unsigned char *b, size_t avail, uint32_t *cp)
{
    size_t n;
    uint32_t c;
    uint32_t min;
    if (b[0] < 0x80) {
        *cp = b[0];
        return 1;
    } else if ((b[0] & 0xE0) == 0xC0) {
        n = 2;
        c = b[0] & 0x1F;
        min = 0x80;
    } else if ((b[0] & 0xF0) == 0xE0) {
        n = 3;
        c = b[0] & 0x0F;
        min = 0x800;
    } else if ((b[0] & 0xF8) == 0xF0) {
        n = 4;
        c = b[0] & 0x07;
        min = 0x10000;
    } else {
        return 0;
    }
    if (n > avail) {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if ((b[i] & 0xC0) != 0x80) {
            return 0;
        }
        c = c << 6 | (b[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        return 0;
    }
    *cp = c;
    return n;
}

// Text the modem converted itself. IRA is ASCII; a byte past it is read as
// UTF-8 where it starts a valid sequence and as Latin-1 where not, so what
// comes out is valid UTF-8 whatever came in.
static void put_raw(sink_t *s, const char *p, size_t len)
{
    const unsigned char *b = (const unsigned char *)p;
    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        size_t n = utf8_decode(b + i, len - i, &cp);
        if (n == 0) {
            cp = b[i];
            n = 1;
        }
        sink_put(s, cp);
        i += n;
    }
}

// `units` UTF-16BE code units in hex. UCS2 proper has no surrogates, but
// phones send emoji as pairs; half a pair becomes U+FFFD.
static void put_utf16(sink_t *s, const char *hex, size_t units)
{
    for (size_t i = 0; i < units; i++) {
        uint32_t u = unit_at(hex, i);
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units) {
            uint32_t lo = unit_at(hex, i + 1);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                sink_put(s, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                i++;
                continue;
            }
        }
        if (u >= 0xD800 && u <= 0xDFFF) {
            u = 0xFFFD;
        }
        sink_put(s, u);
    }
}

// A header string: UCS2 in hex under AT+CSCS="UCS2", else as it came.
static void put_field(sink_t *s, const field_t *f, bool ucs2)
{
    if (ucs2 && is_ucs2_hex(f->s, f->len)) {
        put_utf16(s, f->s, f->len / 4);
    } else {
        put_raw(s, f->s, f->len);
    }
}

// The time stamp is digits and punctuation; some firmware converts it to
// UCS2 along with the address, some leaves it be.
static void put_time(sink_t *s, const field_t *f, bool ucs2)
{
    if (ucs2 && is_ucs2_hex(f->s, f->len)) {
        bool ascii = true;
        for (size_t i = 0; i < f->len / 4; i++) {
            uint32_t u = unit_at(f->s, i);
            if (u < 0x20 || u > 0x7E) {
                ascii = false;
                break;
            }
        }
        if (ascii) {
            put_utf16(s, f->s, f->len / 4);
            return;
        }
    }
    put_raw(s, f->s, f->len);
}

// --- GSM 7-bit default alphabet (TS 23.038 6.2.1) ---

static const uint16_t gsm_base[128] = {
    '@',    0x00A3, '$',    0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC,
    0x00F2, 0x00C7, '\n',   0x00D8, 0x00F8, '\r',   0x00C5, 0x00E5,
    0x0394, '_',    0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,
    0x03A3, 0x0398, 0x039E, ' ',    0x00C6, 0x00E6, 0x00DF, 0x00C9,
    ' ',    '!',    '"',    '#',    0x00A4, '%',    '&',    '\'',
    '(',    ')',    '*',    '+',    ',',    '-',    '.',    '/',
    '0',    '1',    '2',    '3',    '4',    '5',    '6',    '7',
    '8',    '9',    ':',    ';',    '<',    '=',    '>',    '?',
    0x00A1, 'A',    'B',    'C',    'D',    'E',    'F',    'G',
    'H',    'I',    'J',    'K',    'L',    'M',    'N',    'O',
    'P',    'Q',    'R',    'S',    'T',    'U',    'V',    'W',
    'X',    'Y',    'Z',    0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7,
    0x00BF, 'a',    'b',    'c',    'd',    'e',    'f',    'g',
    'h',    'i',    'j',    'k',    'l',    'm',    'n',    'o',
    'p',    'q',    'r',    's',    't',    'u',    'v',    'w',
    'x',    'y',    'z',    0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0,
};

// The character after an escape (0x1B), from the extension table (6.2.1.1).
static uint32_t gsm_ext(unsigned c)
{
    switch (c) {
    case 0x0A: return 0x0C;    // page break, dropped as a control character
    case 0x14: return '^';
    case 0x1B: return ' ';     // escape to a further table: shown as a space
    case 0x28: return '{';
    case 0x29: return '}';
    case 0x2F: return '\\';
    case 0x3C: return '[';
    case 0x3D: return '~';
    case 0x3E: return ']';
    case 0x40: return '|';
    case 0x65: return 0x20AC;  // euro sign
    // A code the table lacks shows as its default alphabet character.
    default:   return gsm_base[c & 0x7F];
    }
}

// Septet i of user data packed as TS 23.038 6.1.2.1.1 packs it: from the
// least significant bit of each octet up.
static unsigned septet_at(const char *hex, size_t octets, size_t i)
{
    size_t bit = i * 7;
    size_t k = bit / 8;
    unsigned shift = (unsigned)(bit % 8);
    unsigned v = octet_at(hex, k) >> shift;
    if (shift > 1 && k + 1 < octets) {
        v |= octet_at(hex, k + 1) << (8 - shift);
    }
    return v & 0x7F;
}

static size_t octets_for_septets(unsigned long septets)
{
    return (size_t)((septets * 7 + 7) / 8);
}

// 7-bit user data with a header of `udh_octets` (UDHL included). The text
// starts at the first septet boundary after the header, past its fill bits.
// <length> counts septets, header included; one that does not match the
// octets given is checked as a count without the header, then ignored.
static void put_gsm7(sink_t *s, const char *hex, size_t octets, size_t udh_octets,
                     bool have_length, unsigned long length)
{
    size_t header = (udh_octets * 8 + 6) / 7;
    size_t avail = octets * 8 / 7;
    size_t total;
    if (have_length && octets_for_septets(length) == octets) {
        total = (size_t)length;
    } else if (have_length && octets_for_septets(length + header) == octets) {
        total = (size_t)length + header;
    } else {
        total = avail;
        // Seven spare bits in the last octet read as a final '@' (0x00).
        if (total > header && total * 7 == octets * 8 && septet_at(hex, octets, total - 1) == 0) {
            total--;
        }
    }
    if (total > avail) {
        total = avail;
    }
    for (size_t i = header; i < total; i++) {
        unsigned c = septet_at(hex, octets, i);
        if (c == 0x1B) {
            if (i + 1 >= total) {
                break;  // an escape with nothing after it
            }
            sink_put(s, gsm_ext(septet_at(hex, octets, ++i)));
        } else {
            sink_put(s, gsm_base[c]);
        }
    }
}

// The concatenation element of a user data header (TS 23.040 9.2.3.24.1 and
// 9.2.3.24.8); other elements are skipped.
static void read_udh(const char *hex, size_t udh_octets, hal_sms_t *out)
{
    unsigned ref = 0;
    unsigned parts = 0;
    unsigned part = 0;
    size_t i = 1;  // past UDHL
    while (i + 2 <= udh_octets) {
        unsigned iei = octet_at(hex, i);
        size_t len = octet_at(hex, i + 1);
        i += 2;
        if (i + len > udh_octets) {
            break;
        }
        if (iei == 0x00 && len == 3) {
            ref = octet_at(hex, i);
            parts = octet_at(hex, i + 1);
            part = octet_at(hex, i + 2);
        } else if (iei == 0x08 && len == 4) {
            ref = octet_at(hex, i) << 8 | octet_at(hex, i + 1);
            parts = octet_at(hex, i + 2);
            part = octet_at(hex, i + 3);
        }
        i += len;
    }
    if (parts <= 1 || part == 0 || part > parts) {
        return;  // a message that is whole, or numbering that cannot be used
    }
    out->concat_ref = (uint16_t)ref;
    out->parts = (uint8_t)parts;
    out->part = (uint8_t)part;
}

enum alphabet { ALPHABET_GSM7, ALPHABET_8BIT, ALPHABET_UCS2 };

// TS 23.038 4: the coding groups of an SMS data coding scheme.
static enum alphabet alphabet_of(unsigned long dcs)
{
    // General data coding (00xx) and automatic deletion (01xx): bits 3..2.
    if ((dcs & 0x80) == 0) {
        switch ((dcs >> 2) & 0x03) {
        case 1:  return ALPHABET_8BIT;
        case 2:  return ALPHABET_UCS2;
        default: return ALPHABET_GSM7;  // 3 is reserved
        }
    }
    switch (dcs & 0xF0) {
    case 0xE0: return ALPHABET_UCS2;    // message waiting, store, UCS2
    case 0xF0: return (dcs & 0x04) ? ALPHABET_8BIT : ALPHABET_GSM7;
    default:   return ALPHABET_GSM7;    // message waiting 0xC0 and 0xD0; reserved
    }
}

static const char *after_line_end(const char *p, const char *end)
{
    if (p < end && *p == '\r') {
        p++;
    }
    if (p < end && *p == '\n') {
        p++;
    }
    return p;
}

static const char *before_line_end(const char *start, const char *end)
{
    if (end > start && end[-1] == '\n') {
        end--;
    }
    if (end > start && end[-1] == '\r') {
        end--;
    }
    return end;
}

// The start of the last line that reads "OK": text in IRA can hold such a
// line too, but the final result comes after it. NULL if there is none.
static const char *final_ok(const char *start, const char *end)
{
    const char *found = NULL;
    for (const char *p = start; p + 2 <= end; p++) {
        if (p[0] == 'O' && p[1] == 'K' && (p == start || p[-1] == '\n')
            && (p + 2 == end || p[2] == '\r' || p[2] == '\n')) {
            found = p;
        }
    }
    return found;
}

esp_err_t a7682e_parse_cmgr(const char *resp, bool ucs2, hal_sms_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!resp) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *head = strstr(resp, "+CMGR:");
    if (!head) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    head += strlen("+CMGR:");
    const char *head_end = head + strcspn(head, "\r\n");

    field_t f[CMGR_FIELDS];
    int n = split_fields(head, head_end, f, CMGR_FIELDS);
    if (n < 2) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const field_t *stat = &f[F_STAT];
    if (field_is(stat, "STO UNSENT") || field_is(stat, "STO SENT")
        || field_is(stat, "2") || field_is(stat, "3")) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!field_is(stat, "REC UNREAD") && !field_is(stat, "REC READ")
        && !field_is(stat, "0") && !field_is(stat, "1")) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    // A status report, a command and a broadcast have a number where a
    // received message has its quoted sender.
    if (!f[F_OA].quoted && field_is_number(&f[F_OA])) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Without AT+CSDH=1 the header stops at <scts>, and a received message
    // is then taken as an SMS-DELIVER in the default alphabet, no header.
    unsigned long fo = 0x04;
    unsigned long dcs = 0;
    unsigned long length = 0;
    int have_fo = n > F_FO ? field_uint(&f[F_FO], 255, &fo) : 0;
    int have_dcs = n > F_DCS ? field_uint(&f[F_DCS], 255, &dcs) : 0;
    int have_length = n > F_LENGTH ? field_uint(&f[F_LENGTH], 65535, &length) : 0;
    if (have_fo < 0 || have_dcs < 0 || have_length < 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if ((fo & FO_MTI_MASK) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    sink_t sender = sink_for(out->sender, sizeof(out->sender), false);
    put_field(&sender, &f[F_OA], ucs2);
    if (n > F_SCTS) {
        sink_t sent_at = sink_for(out->sent_at, sizeof(out->sent_at), false);
        put_time(&sent_at, &f[F_SCTS], ucs2);
    }

    // <data> runs from the line after the header to the CR LF that ends it,
    // before the blank line and OK of the final result. 7-bit text in IRA
    // can hold line ends of its own, so it is not cut at the first.
    const char *end = head_end + strlen(head_end);
    const char *data = after_line_end(head_end, end);
    const char *data_end = final_ok(data, end);
    if (!data_end) {
        data_end = end;
    }
    data_end = before_line_end(data, before_line_end(data, data_end));
    data = after_line_end(data, data_end);
    size_t len = (size_t)(data_end - data);

    sink_t text = sink_for(out->text, sizeof(out->text), true);
    enum alphabet alphabet = alphabet_of(dcs);
    bool udhi = (fo & FO_UDHI) != 0;
    // TS 27.005 3.1: 7-bit text without a header comes in the TE character
    // set, which under UCS2 is hex again; anything else is its octets in hex.
    if (alphabet == ALPHABET_GSM7 && !udhi) {
        field_t body = { data, len, false };
        put_field(&text, &body, ucs2);
        return ESP_OK;
    }
    if (!is_hex(data, len)) {
        put_raw(&text, data, len);  // not hex after all: show it as it came
        return ESP_OK;
    }
    size_t octets = len / 2;
    size_t udh_octets = 0;
    if (udhi && octets > 0) {
        udh_octets = 1 + octet_at(data, 0);
        if (udh_octets > octets) {
            udh_octets = octets;
        }
        read_udh(data, udh_octets, out);
    }
    switch (alphabet) {
    case ALPHABET_GSM7:
        put_gsm7(&text, data, octets, udh_octets, have_length > 0, length);
        break;
    case ALPHABET_8BIT:
        // Latin-1 is as good a guess as any; it keeps ASCII as it is.
        for (size_t i = udh_octets; i < octets; i++) {
            sink_put(&text, octet_at(data, i));
        }
        break;
    case ALPHABET_UCS2:
        put_utf16(&text, data + 2 * udh_octets, (octets - udh_octets) / 2);
        break;
    }
    return ESP_OK;
}
