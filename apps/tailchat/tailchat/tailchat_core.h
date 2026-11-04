/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Chat app's text, with no LVGL in it (host-tested by
 * apps/tailchat/tests/run_tests.sh).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* `text` on one line, its line breaks made spaces, cut to at most
 * `max_chars` characters (the last "..." when cut) and to the buffer,
 * never inside a character. */
void tailchat_one_line(char *out, size_t n, const char *text, size_t max_chars);

/* `text` as a bubble shows it: its lines kept, though never two blank ones
 * in a row nor any at its end, tabs made spaces, cut to `max_chars`
 * characters and `max_lines` lines and to the buffer, never inside a
 * character, with "..." where it was cut. True if it was. */
bool tailchat_excerpt(char *out, size_t n, const char *text, size_t max_chars, size_t max_lines);

/* A length as minutes and seconds, "1:05". */
const char *tailchat_duration(char *out, size_t n, uint32_t ms);

/* A name's monogram, as its avatar shows it: the first letter of each of its
 * first two words, a-z made capitals ("Lena" "L", "ben smith" "BS",
 * "lena-laptop" "L"); "?" for a name with no letter or digit. */
const char *tailchat_initials(const char *name, char *out, size_t n);

/* When a conversation's latest message was written, as the list shows it:
 * the time today ("14:05", or "2:05 PM" in 12 hours), "Yesterday", the
 * weekday within the week before ("Mon"), the day this year ("2 Nov"), else
 * with its year ("2 Nov 2025"). A message from later than now is today's. */
const char *tailchat_when(const struct tm *now, const struct tm *then, bool h24, char *out, size_t n);

/* The time of day alone: "14:05", or "2:05 PM". */
const char *tailchat_clock(const struct tm *then, bool h24, char *out, size_t n);

/* A conversation's day heading: "Today", "Yesterday", the weekday within
 * the week before ("Monday"), the day this year ("Sunday, 2 November"),
 * else with its year ("2 November 2025"). */
const char *tailchat_day(const struct tm *now, const struct tm *then, char *out, size_t n);

bool tailchat_same_day(const struct tm *a, const struct tm *b);

/* Whether a message continues the run of the one before it, drawn close
 * under it with no time of its own between: the same writer, within three
 * minutes (times in ms since 1970). */
#define TAILCHAT_RUN_MS (3 * 60 * 1000)
bool tailchat_joins(bool before_mine, uint64_t before_ts, bool mine, uint64_t ts);

/* What a contact is doing, as a conversation's title says under its name,
 * the first that holds: "typing...", "Not reached yet" (added by address,
 * never reached), "Sending..." (connected, with messages in its outbox),
 * "Waiting to send" (not connected, with messages waiting), "Online"
 * (connected), else the name it last came under on the tailnet. */
typedef struct {
    bool        typing;
    bool        pending;
    bool        waiting;
    bool        connected;
    const char *seen_name;
} tailchat_presence_t;
const char *tailchat_presence(const tailchat_presence_t *p);

/* A voice message's waveform: `count` bars from 1 to `max` high, made from
 * its id, so a message has the same one each time it is drawn. The file's
 * loudness is not known without decoding it. */
void tailchat_waveform(const char *id, uint8_t *bars, size_t count, uint8_t max);

/* The reactions the device offers, in its message sheet. */
#define TAILCHAT_REACTIONS 6
extern const char *const tailchat_reactions[TAILCHAT_REACTIONS];

/* An emoji as the log writes it: "+1", "<3", ...; "*" for one it does not
 * know. */
const char *tailchat_emoji_label(const char *emoji);

/* What a message sent has come to, as its tick shows it. */
typedef enum {
    TAILCHAT_TICK_NONE,     /* received, or a state not known */
    TAILCHAT_TICK_WAITING,  /* in the outbox: a clock */
    TAILCHAT_TICK_DELIVERED,/* acked: one tick */
    TAILCHAT_TICK_READ,     /* a read receipt came: two */
} tailchat_tick_t;
tailchat_tick_t tailchat_tick(bool mine, const char *state);

/* What a contact answered this device's request: -1 nothing yet. */
const char *tailchat_answer_text(int8_t accepted_by_them);

/* A contact's address as the user types it: a tailnet name or address,
 * and ":port" if not the default; spaces around it are dropped. The host
 * into `host`, the port into `*port` (0: the default). False for nothing,
 * a space inside, a port not in 1..65535, or a host too long for `n`. */
bool tailchat_parse_address(const char *text, char *host, size_t n, uint16_t *port);

/* How often the device looks for messages while asleep, in minutes (0
 * off): the setting's value ("Every 15 min", "Every hour", "Off"), and the
 * next of the choices a press goes round (5, 15, 30, 60, off). */
const char *tailchat_check_label(uint32_t minutes, char *out, size_t n);
uint32_t tailchat_next_check_minutes(uint32_t minutes);
