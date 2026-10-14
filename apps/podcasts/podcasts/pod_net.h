/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Podcasts app's HTTP: PodcastIndex's API, and audio.
 *
 * Everything here blocks, so it runs on the app's own threads, never on the
 * LVGL task. ESP-IDF's open/read API does not follow redirects, which every
 * podcast host's tracking links are made of, so pod_net_open() does.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How long one step (connecting, a read) may wait. */
#define POD_NET_TIMEOUT_MS 15000

/* GETs POD_API_BASE + `path`, signed with the key and secret: the body of
 * a 200, NUL-terminated, in PSRAM, for the caller to free(), with its length
 * in `*len`. NULL with what went wrong in `error`, in words for the user:
 * PodcastIndex's own when it refused with some. */
char *pod_net_api_get(const char *path, size_t *len, char *error, size_t error_cap);

typedef struct pod_stream pod_stream_t;

/* pod_net_open()'s error when the server says the byte asked for is past
 * the end of the file (HTTP 416). */
#define POD_NET_PAST_END "Past the end of the episode"

/* Opens `url` to read from byte `from`. `*total` is the whole file's
 * length, 0 when the server does not say; `*start` the byte the stream
 * starts at: `from`, or 0 when the server sends the whole file instead. NULL
 * with `error` set on failure. */
pod_stream_t *pod_net_open(const char *url, int64_t from, int64_t *total, int64_t *start,
                           char *error, size_t error_cap);

/* Up to `len` bytes: how many, 0 at the end, negative when the connection
 * failed or timed out. */
int pod_net_read(pod_stream_t *stream, void *buf, int len);

void pod_net_close(pod_stream_t *stream);

/* Whether a network is up at all. */
bool pod_net_connected(void);
