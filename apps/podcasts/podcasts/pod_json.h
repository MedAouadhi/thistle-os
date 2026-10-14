/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — a JSON reader that walks the text in place.
 *
 * PodcastIndex answers with a few dozen kilobytes of JSON, of which the app
 * keeps a handful of fields an item. A tree of the whole answer would be
 * hundreds of small allocations, and below 4 KB those land in internal RAM,
 * which runs out first (CLAUDE.md). This reader allocates nothing: the caller
 * walks objects and arrays and copies out the values it wants, into buffers
 * of its own, skipping the rest.
 *
 *     pod_json_t j;
 *     pod_json_init(&j, text, len);
 *     char key[32];
 *     if (pod_json_object(&j)) {
 *         while (pod_json_key(&j, key, sizeof key)) {
 *             if (strcmp(key, "title") == 0) pod_json_string(&j, title, sizeof title);
 *             else pod_json_skip(&j);
 *         }
 *     }
 *
 * Malformed text sets `error`, after which every call returns false or reads
 * nothing, so loops like the one above end by themselves.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *p;
    const char *end;
    bool error;
} pod_json_t;

void pod_json_init(pod_json_t *j, const char *text, size_t len);

/* Enters an object or an array: true if the next value is one. Anything else
 * is skipped, and the call returns false. */
bool pod_json_object(pod_json_t *j);
bool pod_json_array(pod_json_t *j);

/* The next member of the object entered: true with its key in `key` (cut to
 * fit), its value next to be read; false at the object's end, which is then
 * passed. Every true must be followed by reading or skipping the value. */
bool pod_json_key(pod_json_t *j, char *key, size_t key_cap);

/* Whether the array entered has another element, next to be read; false at
 * its end, which is then passed. */
bool pod_json_next(pod_json_t *j);

/* Reads a string into `out`, unescaped, as UTF-8 cut to fit between
 * characters. A number or a boolean is read as its text, and null as "".
 * False, with "" in `out`, for an object or an array, which is skipped. */
bool pod_json_string(pod_json_t *j, char *out, size_t cap);

/* Reads a number, dropping any fraction. A string of digits is read as the
 * number it spells, true and false as 1 and 0, null as 0. False, with 0 in
 * `out`, for anything else, which is skipped. */
bool pod_json_int(pod_json_t *j, int64_t *out);

/* Whether the next value is null, which is then passed. */
bool pod_json_null(pod_json_t *j);

/* Passes over the next value, whatever it is. */
void pod_json_skip(pod_json_t *j);
