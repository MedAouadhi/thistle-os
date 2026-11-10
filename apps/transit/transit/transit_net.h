/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Transit app's HTTP, for Providers
 *
 * Blocks, so it runs on the Transit app's task (transit_session.c), never
 * the UI task. Follows redirects, as ESP-IDF's open/read API does not.
 */
#pragma once

#include "transit/transit_provider.h"

#include <stddef.h>

/* How long one step (connecting, a read) may wait. */
#define TRANSIT_NET_TIMEOUT_MS 15000
/* The longest answer read: 64 vehicles of a radar answer are about 300 KB. */
#define TRANSIT_NET_BODY_MAX   (768 * 1024)

/* GETs url: the body of a 200, NUL-terminated, in PSRAM, for the caller to
 * free(), its length in *len. NULL with `error` filled otherwise: no
 * network, no answer, or another status. The URL is logged without its
 * query. */
char *transit_net_get(const char *url, size_t *len, transit_error_t *error);
