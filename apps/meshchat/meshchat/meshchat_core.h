/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the MeshCore app's addresses and words, with no LVGL in it
 * (host-tested by apps/meshchat/tests/run_tests.sh).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A contact's conversation is addressed by the first MESHCHAT_KEY_PREFIX
 * bytes of its key, in hex, as MeshCore shortens keys and the message
 * history names its files. */
#define MESHCHAT_KEY_PREFIX 6
#define MESHCHAT_ADDR_LEN   (MESHCHAT_KEY_PREFIX * 2 + 1)

void meshchat_address(const uint8_t *pub_key, char out[MESHCHAT_ADDR_LEN]);

/* The key prefix an address names; false for one that is not
 * MESHCHAT_KEY_PREFIX bytes of hex. */
bool meshchat_parse_address(const char *addr, uint8_t prefix[MESHCHAT_KEY_PREFIX]);

/* How far a contact is: "in range", "1 hop", "3 hops", or "route unknown"
 * for MESH_PATH_UNKNOWN (0xFF). */
const char *meshchat_route(uint8_t path_len, char *out, size_t n);

/* The longest text a message to the conversation can carry: MeshCore's
 * `max` to a contact; on the Public channel, less "<our name>: ", which
 * goes out in front of it. */
uint32_t meshchat_text_limit(bool channel, size_t self_name_len, uint32_t max);
