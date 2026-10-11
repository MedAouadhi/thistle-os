/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — what the Reader's EPUB code needs from its platform.
 *
 * The EPUB code builds for the firmware, both simulators and a plain host
 * compiler for its tests, so it reaches memory and logging only through here.
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>

/* A book's data goes to PSRAM. Allocations of 4 KB and under otherwise land in
 * internal RAM, of which the T-Deck Max has about 23 KB left once it has
 * booted, and which the display, WiFi and DMA buffers depend on. Parsing one
 * chapter makes thousands of small allocations. */
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"

static inline void *epub_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(size);
}

static inline void *epub_realloc(void *ptr, size_t size)
{
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
    return p ? p : realloc(ptr, size);
}
#else
static inline void *epub_malloc(size_t size) { return malloc(size); }
static inline void *epub_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
#endif

static inline void epub_free(void *ptr) { free(ptr); }

/* A salt for Expat's hash tables, which it would otherwise gather from
 * sources the firmware does not have. Never 0, which tells Expat to. */
#ifdef ESP_PLATFORM
#include "esp_random.h"

static inline unsigned long epub_hash_salt(void)
{
    return (unsigned long)esp_random() | 1u;
}
#else
#include <time.h>

static inline unsigned long epub_hash_salt(void)
{
    return ((unsigned long)rand() << 16 ^ (unsigned long)rand() ^ (unsigned long)time(NULL)) | 1u;
}
#endif

#if __has_include("esp_log.h")
#include "esp_log.h"
#else
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__)
#endif
