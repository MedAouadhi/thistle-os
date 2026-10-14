/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Phone app's words for times and numbers. Internal to the
 * app, and free of LVGL and the kernel so the host tests can build it
 * (tests/run_tests.sh).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* When a call was, as a phone's call list says it, in local time:
 * "14:05" (or "2:05 PM" without `h24`) today, "Yesterday", the weekday
 * ("Mon") within the last six days, "12 Oct" earlier this year and
 * "Oct 2025" before it. A time to come, which only a clock set wrong
 * gives, is dated. "" for 0, a call made before the clock was set. */
void phone_text_when(uint32_t when, time_t now, bool h24, char *out, size_t cap);

/* What a key typed into the dialler adds to the number: a digit, '+', '*' or
 * '#'; 0 for anything else. */
char phone_text_dial_char(uint32_t key);

/* Appends `c` to the number in `number`, of `cap` bytes; false when full. */
bool phone_text_dial_append(char *number, size_t cap, char c);

/* Removes the number's last character; false when it was empty. */
bool phone_text_dial_delete(char *number);

/* Up to two letters for a contact's picture: the first of the first and the
 * last word that begin with a letter ("AS" for "Anna Schmidt", "J" for
 * "Jürgen"), whole UTF-8 characters, ASCII ones in capitals. "" for a name
 * with none, such as a number. */
void phone_text_initials(const char *name, char *out, size_t cap);

/* Whether `text` is all printable ASCII, which Montserrat draws. */
bool phone_text_ascii(const char *text);

/* Whether two numbers are the same for the call list's purposes: equal
 * digits, ignoring spaces and punctuation. */
bool phone_text_same_number(const char *a, const char *b);

/* `number` as it is dialled: '+', digits, '*' and '#', without the spaces,
 * dashes, dots and brackets a card writes it with. */
void phone_text_compact(const char *number, char *out, size_t cap);
