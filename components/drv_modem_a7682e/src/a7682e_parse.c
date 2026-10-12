// SPDX-License-Identifier: BSD-3-Clause
#include "a7682e_parse.h"

#include <stdlib.h>
#include <string.h>

int a7682e_final_result(const char *buf)
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
        if (len == 2 && strncmp(line, "OK", 2) == 0) {
            return 1;
        }
        if ((len == 5 && strncmp(line, "ERROR", 5) == 0)
            || strncmp(line, "+CME ERROR:", 11) == 0
            || strncmp(line, "+CMS ERROR:", 11) == 0) {
            return -1;
        }
        line = eol + 1;
    }
    return 0;
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
