/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Opus at Tailnet chat's settings
 *
 * libopus 1.5.2 itself is opus.h, included here: opus_encode(),
 * opus_decode() (NULL data conceals a lost frame) and opus_*_ctl() are
 * upstream's. This adds encoders and decoders made for speech at 16 kHz mono
 * in 20 ms frames, their state allocated from PSRAM on the device, and a
 * measure of the pseudostack.
 *
 * Every encoder and decoder shares one pseudostack (a scratch buffer libopus
 * pushes its arrays on instead of the C stack): use them from one task at a
 * time. The first encode or decode allocates it, GLOBAL_STACK_SIZE bytes
 * (components/opus/opus.cmake), and it is never freed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "opus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define THISTLE_OPUS_RATE 16000
#define THISTLE_OPUS_CHANNELS 1
/* 20 ms */
#define THISTLE_OPUS_FRAME 320
/* Speech at 16 kHz: 24 kbit/s is wideband SILK, about 60 bytes a frame */
#define THISTLE_OPUS_BITRATE 24000
/* What an S3 can afford: at 240 MHz, a 20 ms frame takes 6.2 ms to encode at
 * complexity 1, 22 ms at libopus' default 9, which cannot keep up. Ticket 01
 * of .scratch/tailnet-chat has the table. */
#define THISTLE_OPUS_COMPLEXITY 1
/* An Opus packet is at most 1275 bytes */
#define THISTLE_OPUS_MAX_PACKET 1275

/* An encoder for speech: OPUS_APPLICATION_VOIP, `bitrate` bits a second,
 * THISTLE_OPUS_COMPLEXITY, VBR, in-band FEC on, expecting 10 % loss. NULL if
 * out of memory. */
OpusEncoder *thistle_opus_encoder_new(int32_t bitrate);
void thistle_opus_encoder_free(OpusEncoder *enc);

OpusDecoder *thistle_opus_decoder_new(void);
void thistle_opus_decoder_free(OpusDecoder *dec);

/* Where codec states made from now on and the pseudostack go: PSRAM (the
 * default) or internal RAM, which is faster and scarce (ticket 01's
 * comments have the numbers). Frees the pseudostack, so call it only while no
 * encoder or decoder is in use; the next encode or decode allocates it
 * again. Off the device there is one heap and it changes nothing. */
void thistle_opus_place(bool internal);

/* The pseudostack: its size, and how much of it any encode or decode has
 * used since it was allocated (0 before the first). */
size_t thistle_opus_scratch_size(void);
size_t thistle_opus_scratch_used(void);

#ifdef __cplusplus
}
#endif
