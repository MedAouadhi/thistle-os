// SPDX-License-Identifier: BSD-3-Clause
#pragma once

/* Ogg/Opus (RFC 3533, RFC 7845) for Tailnet chat's voice messages: one
 * logical stream, mono, its OpusHead and OpusTags, then the audio packets.
 * No I/O and no allocation: the caller hands every buffer in. Host-tested
 * by apps/tailchat/tests/run_tests.sh against docs/tailnet-chat/vectors/. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An Ogg page holds at most 255 lacing values; a writer page this many
 * packets of under 255 bytes each, about a second of 20 ms frames. */
#define TAILCHAT_OGG_PAGE_PACKETS 50

typedef struct {
    uint8_t channels;
    uint32_t input_rate;
    /* Samples at 48 kHz to drop from the start of the decoded audio. */
    uint16_t pre_skip;
    /* The last page's granule position, at 48 kHz. */
    uint64_t last_granule;
    /* Audio packets read. */
    size_t count;
} tailchat_ogg_info_t;

/* The file's length in ms, from its last granule and pre-skip. */
uint32_t tailchat_ogg_ms(const tailchat_ogg_info_t *info);

/* Write the `count` Opus packets in `packets`, `lens[i]` bytes each and one
 * after another, as an Ogg/Opus file of 20 ms frames recorded at 16 kHz
 * mono: each page's granule position counts the frames before its end,
 * 960 samples at 48 kHz each, after `pre_skip`. Into `out`, at most `cap`
 * bytes: the file's length, or 0 if it does not fit. */
size_t tailchat_ogg_write(uint8_t *out, size_t cap, const uint8_t *packets, const uint16_t *lens,
                          size_t count, uint16_t pre_skip, uint32_t serial);

/* Read an Ogg/Opus file: every page's CRC checked, its audio packets copied
 * one after another into `packets` (at most `cap` bytes) with their lengths
 * in `lens` (at most `max`). False if it is not a whole Ogg/Opus file or
 * does not fit. */
bool tailchat_ogg_read(const uint8_t *file, size_t len, uint8_t *packets, size_t cap, uint16_t *lens,
                       size_t max, tailchat_ogg_info_t *info);

#ifdef __cplusplus
}
#endif
