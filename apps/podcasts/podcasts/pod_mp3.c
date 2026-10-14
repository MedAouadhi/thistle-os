/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — minimp3, built once, for the player.
 *
 * Layer III only: podcasts are MP3. The 16 KB of scratch each frame needs is
 * a static in PSRAM rather than on the decoding thread's stack, which is in
 * internal RAM (see third_party/minimp3/README.md); the player decodes on one
 * thread at a time.
 */
#include "esp_attr.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_SCRATCH_STORAGE static EXT_RAM_BSS_ATTR
#include "third_party/minimp3/minimp3.h"
