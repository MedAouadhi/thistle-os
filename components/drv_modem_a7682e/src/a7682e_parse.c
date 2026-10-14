// SPDX-License-Identifier: BSD-3-Clause
#include "a7682e_parse.h"

#include <stdlib.h>
#include <string.h>

static bool line_is(const char *line, size_t len, const char *word)
{
    return len == strlen(word) && strncmp(line, word, len) == 0;
}

/* The result codes that end a call attempt without a call (V.25ter 6.3.1,
 * the manual's 28.1). */
static bool is_call_failure(const char *line, size_t len)
{
    return line_is(line, len, "NO CARRIER") || line_is(line, len, "BUSY")
        || line_is(line, len, "NO ANSWER") || line_is(line, len, "NO DIALTONE");
}

static int final_result(const char *buf, bool call)
{
    if (!buf) {
        return 0;
    }
    const char *line = buf;
    const char *eol;
    while ((eol = strchr(line, '\n')) != NULL) {
        size_t len = (size_t)(eol - line);
        if (len > 0 && line[len - 1] == '\r') {
            len--;
        }
        if (line_is(line, len, "OK")) {
            return 1;
        }
        if (line_is(line, len, "ERROR")
            || strncmp(line, "+CME ERROR:", 11) == 0
            || strncmp(line, "+CMS ERROR:", 11) == 0
            || (call && is_call_failure(line, len))) {
            return -1;
        }
        line = eol + 1;
    }
    return 0;
}

int a7682e_final_result(const char *buf)
{
    return final_result(buf, false);
}

int a7682e_call_final_result(const char *buf)
{
    return final_result(buf, true);
}

hal_sim_state_t a7682e_parse_sim(const char *resp)
{
    if (!resp) {
        return HAL_SIM_UNKNOWN;
    }
    const char *cpin = strstr(resp, "+CPIN:");
    if (cpin) {
        cpin += strlen("+CPIN:");
        while (*cpin == ' ') {
            cpin++;
        }
        if (strncmp(cpin, "READY", 5) == 0) {
            return HAL_SIM_READY;
        }
        // PIN2 and PUK2 guard only a few SIM records; the card is usable.
        if (strncmp(cpin, "SIM PIN2", 8) == 0 || strncmp(cpin, "SIM PUK2", 8) == 0) {
            return HAL_SIM_READY;
        }
        if (strncmp(cpin, "SIM PIN", 7) == 0) {
            return HAL_SIM_PIN_REQUIRED;
        }
        if (strncmp(cpin, "SIM PUK", 7) == 0) {
            return HAL_SIM_PUK_REQUIRED;
        }
        // The Simcom URC forms, which some firmware also answers with.
        if (strncmp(cpin, "SIM REMOVED", 11) == 0 || strncmp(cpin, "NOT INSERTED", 12) == 0) {
            return HAL_SIM_ABSENT;
        }
        if (strncmp(cpin, "NOT READY", 9) == 0) {
            return HAL_SIM_UNKNOWN;
        }
        // PH-SIM PIN, PH-NET PIN: the modem is locked to another SIM or
        // network, which no setting here can unlock.
        return HAL_SIM_ERROR;
    }

    const char *cme = strstr(resp, "+CME ERROR:");
    if (!cme) {
        return HAL_SIM_UNKNOWN;
    }
    cme += strlen("+CME ERROR:");
    while (*cme == ' ') {
        cme++;
    }
    if (strncmp(cme, "SIM not inserted", 16) == 0) {
        return HAL_SIM_ABSENT;
    }
    if (strncmp(cme, "SIM busy", 8) == 0) {
        return HAL_SIM_UNKNOWN;
    }
    if (strncmp(cme, "SIM PIN required", 16) == 0) {
        return HAL_SIM_PIN_REQUIRED;
    }
    if (strncmp(cme, "SIM PUK required", 16) == 0) {
        return HAL_SIM_PUK_REQUIRED;
    }
    char *end = NULL;
    long code = strtol(cme, &end, 10);
    if (end != cme) {
        switch (code) {
        case 10: return HAL_SIM_ABSENT;
        case 11: return HAL_SIM_PIN_REQUIRED;
        case 12: return HAL_SIM_PUK_REQUIRED;
        case 14: return HAL_SIM_UNKNOWN;  // SIM busy
        default: return HAL_SIM_ERROR;
        }
    }
    return HAL_SIM_ERROR;
}

static hal_cell_reg_t reg_from_stat(long stat)
{
    switch (stat) {
    case 0:  return HAL_CELL_REG_NOT_REGISTERED;
    case 1:  return HAL_CELL_REG_HOME;
    case 2:  return HAL_CELL_REG_SEARCHING;
    case 3:  return HAL_CELL_REG_DENIED;
    case 5:  return HAL_CELL_REG_ROAMING;
    case 6:  // SMS only, home
    case 7:  // SMS only, roaming
    case 11: // emergency bearer only
        return HAL_CELL_REG_NOT_REGISTERED;
    default: return HAL_CELL_REG_UNKNOWN;
    }
}

hal_cell_reg_t a7682e_parse_reg(const char *resp)
{
    if (!resp) {
        return HAL_CELL_REG_UNKNOWN;
    }
    const char *p = strstr(resp, "REG:");
    if (!p || p == resp || p[-1] < 'A' || p[-1] > 'Z') {
        return HAL_CELL_REG_UNKNOWN;
    }
    p += strlen("REG:");
    char *end = NULL;
    long first = strtol(p, &end, 10);
    if (end == p) {
        return HAL_CELL_REG_UNKNOWN;
    }
    if (*end != ',') {
        return reg_from_stat(first);
    }
    // "<n>,<stat>" — but a URC with location reads "<stat>,<lac>,..." and its
    // second field is quoted, so a quote after the comma means `first` was
    // the status.
    const char *second = end + 1;
    while (*second == ' ') {
        second++;
    }
    if (*second == '"') {
        return reg_from_stat(first);
    }
    long stat = strtol(second, &end, 10);
    if (end == second) {
        return HAL_CELL_REG_UNKNOWN;
    }
    return reg_from_stat(stat);
}

static int reg_rank(hal_cell_reg_t reg)
{
    switch (reg) {
    case HAL_CELL_REG_HOME:
    case HAL_CELL_REG_ROAMING:        return 4;
    case HAL_CELL_REG_SEARCHING:      return 3;
    case HAL_CELL_REG_DENIED:         return 2;
    case HAL_CELL_REG_NOT_REGISTERED: return 1;
    case HAL_CELL_REG_UNKNOWN:
    default:                          return 0;
    }
}

hal_cell_reg_t a7682e_reg_best(hal_cell_reg_t a, hal_cell_reg_t b)
{
    return reg_rank(b) > reg_rank(a) ? b : a;
}

bool a7682e_parse_operator(const char *resp, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (!resp) {
        return false;
    }
    const char *p = strstr(resp, "+COPS:");
    if (!p) {
        return false;
    }
    const char *open = strchr(p, '"');
    if (!open) {
        return false;
    }
    open++;
    const char *close = strchr(open, '"');
    if (!close || close == open) {
        return false;
    }
    size_t len = (size_t)(close - open);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, open, len);
    out[len] = '\0';
    return true;
}

bool a7682e_parse_context_apn(const char *resp, int cid, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (!resp) {
        return false;
    }
    for (const char *p = strstr(resp, "+CGDCONT:"); p; p = strstr(p + 1, "+CGDCONT:")) {
        char *end;
        long id = strtol(p + 9, &end, 10);
        if (end == p + 9 || id != cid || *end != ',') {
            continue;
        }
        /* Past the quoted PDP type to the quoted APN. */
        if (end[1] != '"') {
            return false;
        }
        const char *type_end = strchr(end + 2, '"');
        if (!type_end || type_end[1] != ',' || type_end[2] != '"') {
            return false;
        }
        const char *open = type_end + 3;
        const char *close = strchr(open, '"');
        if (!close || close == open) {
            return false;
        }
        size_t len = (size_t)(close - open);
        if (len >= out_size) {
            len = out_size - 1;
        }
        memcpy(out, open, len);
        out[len] = '\0';
        return true;
    }
    return false;
}

/* Days from 1970-01-01 to a date of the Gregorian calendar (Howard Hinnant's
 * days_from_civil). */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int days_in_month(int y, int m)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : days[m - 1];
}

bool a7682e_parse_cclk(const char *resp, int64_t *utc, int *offset_min)
{
    if (!resp || !utc || !offset_min) {
        return false;
    }
    const char *p = strstr(resp, "+CCLK:");
    if (!p) {
        return false;
    }
    p += strlen("+CCLK:");
    while (*p == ' ') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    /* "yy/MM/dd,hh:mm:ss", each field two digits. */
    static const char seps[] = "//,::";
    int field[6];
    p++;
    for (int i = 0; i < 6; i++) {
        if (!is_digit(p[0]) || !is_digit(p[1]) || (i < 5 && p[2] != seps[i])) {
            return false;
        }
        field[i] = (p[0] - '0') * 10 + (p[1] - '0');
        p += i < 5 ? 3 : 2;
    }
    int yy = field[0], mo = field[1], dd = field[2], hh = field[3], mi = field[4], ss = field[5];
    if (*p != '+' && *p != '-') {
        return false;
    }
    char *end;
    long quarters = strtol(p, &end, 10);
    if (end == p + 1 || *end != '"' || quarters < -96 || quarters > 96) {
        return false;
    }
    int year = yy < 70 ? 2000 + yy : 1900 + yy;
    if (mo < 1 || mo > 12 || dd < 1 || dd > days_in_month(year, mo)
        || hh > 23 || mi > 59 || ss > 59) {
        return false;
    }
    int64_t local = days_from_civil(year, mo, dd) * 86400 + hh * 3600 + mi * 60 + ss;
    *offset_min = (int)quarters * 15;
    *utc = local - (int64_t)*offset_min * 60;
    return true;
}

/* An unsigned integer field, after any spaces; *p is left past it. */
static bool clcc_int(const char **p, long *out)
{
    const char *s = *p;
    while (*s == ' ') {
        s++;
    }
    if (!is_digit(*s)) {
        return false;
    }
    char *end;
    *out = strtol(s, &end, 10);
    *p = end;
    return true;
}

/* One +CLCC line, from past its "+CLCC:" to `eol`: 1 for a voice call, read
 * into *call; 0 for a call of another bearer; -1 for a line that cannot be
 * read. */
static int clcc_line(const char *p, const char *eol, hal_call_t *call)
{
    /* <id>,<dir>,<stat>,<mode>,<mpty> */
    long field[5];
    for (int i = 0; i < 5; i++) {
        if (i > 0) {
            while (*p == ' ') {
                p++;
            }
            if (*p != ',') {
                return -1;
            }
            p++;
        }
        if (!clcc_int(&p, &field[i]) || p > eol) {
            return -1;
        }
    }
    long id = field[0], dir = field[1], stat = field[2], mode = field[3];
    if (id < 1 || id > 255 || (dir != 0 && dir != 1)
        || stat < HAL_CALL_ACTIVE || stat > HAL_CALL_RELEASED) {
        return -1;
    }
    if (mode != 0) {
        return 0;
    }

    memset(call, 0, sizeof(*call));
    call->id = (uint8_t)id;
    call->incoming = dir == 1;
    call->state = (hal_call_state_t)stat;

    /* ,"<number>": absent while the network has not said, "" when withheld.
     * Only printable characters are kept: the number is shown and dialled. */
    while (p < eol && *p == ' ') {
        p++;
    }
    if (p == eol || *p != ',') {
        return p == eol ? 1 : -1;
    }
    p++;
    while (p < eol && *p == ' ') {
        p++;
    }
    if (p == eol || *p != '"') {
        return 1;
    }
    size_t n = 0;
    for (p++; p < eol && *p != '"'; p++) {
        if (*p >= 0x20 && *p <= 0x7e && n < sizeof(call->number) - 1) {
            call->number[n++] = *p;
        }
    }
    call->number[n] = '\0';
    return p < eol ? 1 : -1;  // a number whose closing quote is missing
}

int a7682e_parse_clcc(const char *resp, hal_call_t *calls, int max)
{
    if (!resp) {
        return -1;
    }
    int listed = 0;
    for (const char *line = resp; *line;) {
        const char *eol = line + strcspn(line, "\r\n");
        if ((size_t)(eol - line) >= 6 && strncmp(line, "+CLCC:", 6) == 0) {
            hal_call_t call;
            int got = clcc_line(line + 6, eol, &call);
            if (got < 0) {
                return -1;
            }
            if (got > 0) {
                if (calls && listed < max) {
                    calls[listed] = call;
                }
                listed++;
            }
        }
        line = *eol ? eol + 1 : eol;
    }
    return listed;
}
