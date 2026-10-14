/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the PodcastIndex key and secret.
 *
 * From /sdcard/config/podcasts.json when it has them, which the app's setup
 * page writes:
 *
 *     {"api_key": "UXKCGDSYGUUEVQJSYDZH", "api_secret": "yzJe2eE7XV-..."}
 *
 * and, optionally, "api_url": another server that speaks PodcastIndex's
 * API, in place of POD_API_BASE (the simulator's tests run one).
 *
 * Without a key and secret there, from the build: CMake reads
 * PODCASTINDEX_API_KEY and PODCASTINDEX_SECRET from the repository's .env,
 * which git ignores, into a header in the build directory
 * (credentials.cmake). Neither is ever written inside the source tree.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define POD_CRED_KEY_MAX    64
#define POD_CRED_SECRET_MAX 96

/* The key and secret to sign requests with: false when there are none. */
bool pod_cred_get(char *key, size_t key_cap, char *secret, size_t secret_cap);

/* The API's base URL, no trailing slash: the card's "api_url", or
 * POD_API_BASE. */
void pod_cred_api_base(char *out, size_t cap);

/* Whether the card has its own, and whether the build has any. */
bool pod_cred_on_card(void);
bool pod_cred_built_in(void);

/* Writes the card's. Empty strings remove the file, which brings back the
 * build's. */
bool pod_cred_save(const char *key, const char *secret);
