// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// Host tests for the arguments the A7682E's call commands carry.

#include "a7682e_call_validation.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_numbers(void)
{
    const char *valid[] = {
        "+4917612345678",
        "017612345678",
        "112",
        "1",
        "*31#0301234567",   // withhold this one call's number (a service code)
        "#31#0301234567",
        "*100#",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        assert(a7682e_call_number_is_valid(valid[i]));
    }

    const char *invalid[] = {
        "",
        "+",
        "++49",
        "49+176",
        "0176 1234",
        "0176-1234",
        "0176;H",           // a second command after the dial string
        "0176;\rAT+CFUN=0",
        "0176\r",
        "0176\n",
        "0176I",            // a GSM modifier: CLIR for this call
        "0176G",
        "0176A",            // a letter ATD would dial
        "0176P1",           // pause and DTMF after it
        "0176,1",
        "\"0176\"",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(!a7682e_call_number_is_valid(invalid[i]));
    }
    assert(!a7682e_call_number_is_valid(NULL));

    // HAL_CALL_NUMBER_MAX characters, the '+' counted, and no more.
    char longest[HAL_CALL_NUMBER_MAX + 1];
    memset(longest, '1', HAL_CALL_NUMBER_MAX);
    longest[HAL_CALL_NUMBER_MAX] = '\0';
    assert(a7682e_call_number_is_valid(longest));
    longest[0] = '+';
    assert(a7682e_call_number_is_valid(longest));

    char overlong[HAL_CALL_NUMBER_MAX + 2];
    memset(overlong, '1', HAL_CALL_NUMBER_MAX + 1);
    overlong[HAL_CALL_NUMBER_MAX + 1] = '\0';
    assert(!a7682e_call_number_is_valid(overlong));
    overlong[0] = '+';
    assert(!a7682e_call_number_is_valid(overlong));
}

static void test_dtmf(void)
{
    const char tones[] = "0123456789*#ABCD";
    for (size_t i = 0; i < sizeof(tones) - 1; i++) {
        assert(a7682e_call_dtmf_is_valid(tones[i]));
    }
    const char not_tones[] = "abcdEFZ+, \";\r\n";
    for (size_t i = 0; i < sizeof(not_tones) - 1; i++) {
        assert(!a7682e_call_dtmf_is_valid(not_tones[i]));
    }
    assert(!a7682e_call_dtmf_is_valid('\0'));
}

static void test_gain(void)
{
    // The ends, and the module's own default in the middle.
    assert(a7682e_call_gain_for_volume(0) == 0);
    assert(a7682e_call_gain_for_volume(100) == 7);
    assert(a7682e_call_gain_for_volume(50) == 4);

    // Each step taken at its midpoint: 100/7 percent apiece.
    assert(a7682e_call_gain_for_volume(7) == 0);
    assert(a7682e_call_gain_for_volume(8) == 1);
    assert(a7682e_call_gain_for_volume(21) == 1);
    assert(a7682e_call_gain_for_volume(22) == 2);
    assert(a7682e_call_gain_for_volume(92) == 6);
    assert(a7682e_call_gain_for_volume(93) == 7);

    // Never past the top, and never backwards.
    assert(a7682e_call_gain_for_volume(101) == 7);
    assert(a7682e_call_gain_for_volume(255) == 7);
    for (int p = 1; p <= 255; p++) {
        int g = a7682e_call_gain_for_volume((uint8_t)p);
        assert(g >= a7682e_call_gain_for_volume((uint8_t)(p - 1)));
        assert(g >= 0 && g <= 7);
    }
}

int main(void)
{
    test_numbers();
    test_dtmf();
    test_gain();
    puts("a7682e call argument tests passed");
    return 0;
}
