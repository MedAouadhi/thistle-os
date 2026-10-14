/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Podcasts app's text: descriptions, durations, dates, sizes.
 *
 * Every function writes at most `cap` bytes, NUL included, and cuts UTF-8
 * between characters.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* A podcast's or an episode's description, which feeds write in HTML, as
 * plain text: tags dropped, paragraphs and line breaks as one blank line or a
 * newline, entities decoded, runs of spaces made one, the ends trimmed. */
void pod_text_from_html(const char *html, char *out, size_t cap);

/* Copies `src`, cut to fit between characters. */
void pod_text_copy(char *out, size_t cap, const char *src);

/* A clock: "4:05", "1:02:03". Negative reads as 0. */
void pod_text_clock(int32_t seconds, char *out, size_t cap);

/* A length in words: "45 min", "1 h 5 min", "2 h", "40 s"; "" for 0. */
void pod_text_duration(int32_t seconds, char *out, size_t cap);

/* When something was published, from `now`, both in Unix seconds, in the
 * local zone: "Today", "Yesterday", "3 days ago" within a week, "12 Oct" this
 * year, "12 Oct 2025" before it. "" for 0. */
void pod_text_date(int64_t when, int64_t now, char *out, size_t cap);

/* "512 KB", "28 MB", "1.2 GB". */
void pod_text_size(int64_t bytes, char *out, size_t cap);

/* `in` percent-encoded for a query string: all but A-Z a-z 0-9 - _ . ~.
 * False, with "" in `out`, if it does not fit. */
int pod_text_url_encode(const char *in, char *out, size_t cap);
