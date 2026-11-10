/* SPDX-License-Identifier: BSD-3-Clause
 * The host tests' stand-in for ESP-IDF's heap_caps: plain malloc. */
#pragma once

#include <stdlib.h>
#include <stdint.h>

#define MALLOC_CAP_SPIRAM (1 << 10)

static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}
