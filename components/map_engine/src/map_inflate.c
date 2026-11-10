/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — raw DEFLATE for Map Pack tiles
 *
 * On the device, the ESP32-S3 ROM's inflater (miniz's tinfl), which costs
 * no flash. The simulator and host tests have no ROM, and use uzlib, which
 * the Reader already builds (apps/reader/third_party/uzlib).
 *
 * Either way the decompressor's state, 11 KB for tinfl and 1.2 KB for
 * uzlib, is on the heap, never the caller's stack, and a stream must end
 * exactly at the tile's last byte.
 */
#include "map_engine/map_pack.h"

#include <stdlib.h>
#include <string.h>

#ifndef ESP_PLATFORM

#include "uzlib.h"

bool map_inflate(const uint8_t *src, size_t src_len, uint8_t *out, size_t out_len)
{
    static bool ready;
    if (!ready) {
        uzlib_init();
        ready = true;
    }
    /* A byte of room past the tile: uzlib stops once its buffer is full,
     * before the block's end code, so a stream that runs on would pass
     * for a tile as long as its first bytes were right. tinfl, on the
     * device, refuses it; with the room, so does this. */
    struct uzlib_uncomp *d = calloc(1, sizeof *d);
    uint8_t *room = malloc(out_len + 1);
    if (!d || !room) {
        free(d);
        free(room);
        return false;
    }
    uzlib_uncompress_init(d, NULL, 0);
    d->source       = src;
    d->source_limit = src + src_len;
    d->dest_start   = room;
    d->dest         = room;
    d->dest_limit   = room + out_len + 1;
    int res = uzlib_uncompress(d);
    /* eof: it wanted more than there is. */
    bool ok = res == TINF_DONE && d->dest == room + out_len && !d->eof;
    if (ok) memcpy(out, room, out_len);
    free(room);
    free(d);
    return ok;
}

#else

#include "esp_heap_caps.h"
#if __has_include("miniz.h")
#include "miniz.h"
#else
#include "rom/miniz.h"
#endif

bool map_inflate(const uint8_t *src, size_t src_len, uint8_t *out, size_t out_len)
{
    tinfl_decompressor *d = heap_caps_malloc(sizeof *d, MALLOC_CAP_SPIRAM);
    if (!d) d = malloc(sizeof *d);
    if (!d) return false;
    tinfl_init(d);
    size_t in = src_len, made = out_len;
    /* No TINFL_FLAG_PARSE_ZLIB_HEADER: the payload is raw DEFLATE. No
     * TINFL_FLAG_HAS_MORE_INPUT: this is all of it. */
    tinfl_status status = tinfl_decompress(d, src, &in, out, out, &made,
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(d);
    return status == TINFL_STATUS_DONE && made == out_len;
}

#endif
