/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — libopus' allocation, through thistle_opus.c
 *
 * libopus includes this from celt/os_support.h when CUSTOM_SUPPORT is
 * defined (components/opus/opus.cmake). Its codec states go to PSRAM on the
 * device, and its pseudostack too, filled with a pattern so that
 * thistle_opus_scratch_used() can tell how deep it went.
 */
#ifndef THISTLE_OPUS_CUSTOM_SUPPORT_H
#define THISTLE_OPUS_CUSTOM_SUPPORT_H

#include <stddef.h>

void *thistle_opus_alloc(size_t size);
void *thistle_opus_realloc(void *ptr, size_t size);
void *thistle_opus_alloc_scratch(size_t size);
void thistle_opus_free(void *ptr);

#define OVERRIDE_OPUS_ALLOC
#define OVERRIDE_OPUS_REALLOC
#define OVERRIDE_OPUS_ALLOC_SCRATCH
#define OVERRIDE_OPUS_FREE

static inline void *opus_alloc(size_t size) { return thistle_opus_alloc(size); }
static inline void *opus_realloc(void *ptr, size_t size) { return thistle_opus_realloc(ptr, size); }
static inline void *opus_alloc_scratch(size_t size) { return thistle_opus_alloc_scratch(size); }
static inline void opus_free(void *ptr) { thistle_opus_free(ptr); }

#endif
