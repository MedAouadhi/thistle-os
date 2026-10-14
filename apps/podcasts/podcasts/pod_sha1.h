/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — SHA-1, for PodcastIndex's Authorization header.
 *
 * The API wants sha1(key + secret + time) in lower-case hex on every request.
 * That is all SHA-1 is used for here: it signs nothing of ours, and the
 * simulator has no mbedTLS to borrow it from.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

void pod_sha1(const void *data, size_t len, uint8_t digest[20]);

/* The digest of `text` as 40 lower-case hex digits and a NUL. */
void pod_sha1_hex(const char *text, char out[41]);
