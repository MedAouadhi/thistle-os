// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// The arguments of the A7682E's call commands, checked before they are put
// into a command line, or mapped to what the module takes. Forms are those of
// the A76XX Series AT Command Manual V1.09: ATD (2.2.1), AT+VTS (7.2.14) and
// AT+COUTGAIN (7.2.22).

#pragma once

#include "hal/network.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A number to dial: an optional '+', then at least one of 0-9, '*' and '#',
 * HAL_CALL_NUMBER_MAX characters at most, the '+' counted. ATD would also
 * take the letters A-C and the GSM modifiers I, i, G and g, and AT syntax
 * after a ';' would be run as a further command, so the number is held to
 * the characters a keypad has. */
static inline bool a7682e_call_number_is_valid(const char *number)
{
    if (!number) {
        return false;
    }

    size_t length = 0;
    while (length <= HAL_CALL_NUMBER_MAX && number[length] != '\0') {
        length++;
    }
    if (length == 0 || length > HAL_CALL_NUMBER_MAX) {
        return false;
    }

    size_t index = number[0] == '+' ? 1U : 0U;
    if (index == length) {
        return false;
    }
    for (; index < length; index++) {
        char c = number[index];
        if (!((c >= '0' && c <= '9') || c == '*' || c == '#')) {
            return false;
        }
    }
    return true;
}

/* One DTMF tone AT+VTS sends: 0-9, '*', '#' or A-D. */
static inline bool a7682e_call_dtmf_is_valid(char digit)
{
    return (digit >= '0' && digit <= '9') || digit == '*' || digit == '#'
        || (digit >= 'A' && digit <= 'D');
}

/* The module's output gain, 0 to 7 (4 by default), for a volume of 0 to 100
 * percent, rounded to the nearest step; above 100 counts as 100. */
static inline int a7682e_call_gain_for_volume(uint8_t percent)
{
    unsigned p = percent > 100 ? 100U : percent;
    return (int)((p * 7U + 50U) / 100U);
}
