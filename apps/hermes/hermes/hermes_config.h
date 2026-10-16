/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — where the Hermes app finds the bridge.
 *
 * The card's /sdcard/config/hermes.json when it names one, which the app's
 * setup page writes:
 *
 *     {"bridge_url": "ws://100.125.204.43:8765"}
 *
 * and otherwise the build's: CMake reads HERMES_BRIDGE_URL from the
 * repository's .env into a header in the build directory (bridge_url.cmake).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* The bridge's address: false when there is none. */
bool hermes_config_url(char *out, size_t cap);

/* The build's, or "". */
const char *hermes_config_built_in(void);

/* Writes the card's. "" removes the file, which brings back the build's. */
bool hermes_config_save(const char *url);
