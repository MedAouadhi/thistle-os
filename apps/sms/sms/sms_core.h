/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the SMS app's phone numbers, with no LVGL in it (host-tested
 * by apps/sms/tests/run_tests.sh).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* A number as the modem takes it, from one as people write it: the spaces,
 * dashes, dots, slashes and brackets dropped ("+49 (160) 987-6543" is
 * "+491609876543"). False, `out` empty, if what is left is not '+' and
 * digits, or digits alone, or does not fit. */
bool sms_number(const char *typed, char *out, size_t n);

/* Whether two numbers are one: by their last 9 digits when both have that
 * many, so "+49 176 12345678" is "0176 12345678"; else by all of them. A
 * sender's name, such as "DHL", matches only itself. */
bool sms_numbers_match(const char *a, const char *b);
