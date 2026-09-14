// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Internal helper kept separate so the legacy C implementation's packed-row
 * contract can be exercised on the host. Source rows are byte-padded.
 *
 * Either axis can be mirrored on the way in. The window manager and the input
 * stack always work in ordinary top-left portrait coordinates; which way the
 * panel is mounted, and which way its scan runs, is accounted for here, at the
 * panel boundary, so that glyphs keep their normal direction everywhere above
 * this line. */
static inline void epaper_blit_packed_rect_flipped(
    uint8_t *framebuffer,
    size_t framebuffer_width,
    size_t framebuffer_height,
    uint16_t x1,
    uint16_t y1,
    uint16_t x2,
    uint16_t y2,
    const uint8_t *color_data,
    bool flip_x,
    bool flip_y)
{
    size_t src_width = (size_t)x2 - x1 + 1;
    size_t src_row_bytes = (src_width + 7) / 8;

    for (uint16_t row = y1; row <= y2; row++) {
        const uint8_t *src_row = color_data + (size_t)(row - y1) * src_row_bytes;
        for (uint16_t col = x1; col <= x2; col++) {
            size_t src_bit = (size_t)col - x1;
            uint8_t value = (src_row[src_bit >> 3] >> (7 - (src_bit & 7))) & 1;
            size_t dst_row = flip_y ? framebuffer_height - 1 - row : row;
            size_t dst_col = flip_x ? framebuffer_width - 1 - col : col;
            size_t dst_bit = dst_row * framebuffer_width + dst_col;
            uint8_t mask = 0x80u >> (dst_bit & 7);

            if (value) framebuffer[dst_bit >> 3] |= mask;
            else       framebuffer[dst_bit >> 3] &= (uint8_t)~mask;
        }
    }
}
