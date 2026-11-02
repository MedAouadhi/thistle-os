/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Opus at Tailnet chat's settings (thistle_opus.h)
 */
#include "thistle_opus.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "custom_support.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

/* What the pseudostack is filled with until libopus writes over it */
#define SCRATCH_FILL 0xA5

static unsigned char *s_scratch;
static size_t s_scratch_size;
static bool s_internal;

/* libopus' pseudostack (celt/stack_alloc.h, NONTHREADSAFE_PSEUDOSTACK) */
extern char *global_stack;
extern char *scratch_ptr;

/* PSRAM on the device, the internal heap if there is none, unless
 * thistle_opus_place() asked for internal RAM */
void *thistle_opus_alloc(size_t size)
{
#ifdef ESP_PLATFORM
    void *p = s_internal ? NULL : heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return malloc(size);
#endif
}

void *thistle_opus_realloc(void *ptr, size_t size)
{
#ifdef ESP_PLATFORM
    void *p = s_internal ? NULL : heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return realloc(ptr, size);
#endif
}

void thistle_opus_place(bool internal)
{
    s_internal = internal;
    if (s_scratch) {
        free(s_scratch);
        s_scratch = NULL;
        s_scratch_size = 0;
        global_stack = NULL;
        scratch_ptr = NULL;
    }
}

void thistle_opus_free(void *ptr)
{
    free(ptr);
}

/* libopus asks for its pseudostack once, at the first encode or decode, and
 * keeps it. */
void *thistle_opus_alloc_scratch(size_t size)
{
    unsigned char *p = thistle_opus_alloc(size);
    if (p) {
        memset(p, SCRATCH_FILL, size);
        s_scratch = p;
        s_scratch_size = size;
    }
    return p;
}

size_t thistle_opus_scratch_size(void)
{
    return s_scratch_size;
}

/* The pseudostack grows up from its start; the highest byte libopus wrote
 * is how deep it went. A word it wrote that happens to hold the fill reads
 * as unused, which can only make this a few bytes short. */
size_t thistle_opus_scratch_used(void)
{
    size_t n = s_scratch_size;
    while (n > 0 && s_scratch[n - 1] == SCRATCH_FILL) {
        n--;
    }
    return n;
}

OpusEncoder *thistle_opus_encoder_new(int32_t bitrate)
{
    int err = OPUS_OK;
    OpusEncoder *enc = opus_encoder_create(THISTLE_OPUS_RATE, THISTLE_OPUS_CHANNELS,
                                           OPUS_APPLICATION_VOIP, &err);
    if (!enc || err != OPUS_OK) {
        return NULL;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(THISTLE_OPUS_COMPLEXITY));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(10));
    return enc;
}

void thistle_opus_encoder_free(OpusEncoder *enc)
{
    opus_encoder_destroy(enc);
}

OpusDecoder *thistle_opus_decoder_new(void)
{
    int err = OPUS_OK;
    OpusDecoder *dec = opus_decoder_create(THISTLE_OPUS_RATE, THISTLE_OPUS_CHANNELS, &err);
    return err == OPUS_OK ? dec : NULL;
}

void thistle_opus_decoder_free(OpusDecoder *dec)
{
    opus_decoder_destroy(dec);
}

/* Who has Opus now; NULL while nobody does. */
static _Atomic(const char *) s_holder;

bool thistle_opus_take(const char *who, const char **holder)
{
    const char *none = NULL;
    if (atomic_compare_exchange_strong(&s_holder, &none, who)) return true;
    if (holder) *holder = none;
    return false;
}

void thistle_opus_give(void)
{
    atomic_store(&s_holder, NULL);
}
