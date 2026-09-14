// SPDX-License-Identifier: BSD-3-Clause

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/packed_blit.h"

#define WIDTH 240
#define HEIGHT 10
#define ROW_BYTES (WIDTH / 8)

/* Neither axis mirrored: the source lands where its coordinates say. */
static void blit(uint8_t *framebuffer,
                 uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                 const uint8_t *color_data)
{
    epaper_blit_packed_rect_flipped(framebuffer, WIDTH, HEIGHT,
                                    x1, y1, x2, y2, color_data, false, false);
}

int main(void)
{
    uint8_t framebuffer[ROW_BYTES * HEIGHT] = {0};

    const uint8_t three_by_two[] = {0xA0, 0x40};
    blit(framebuffer, 1, 2, 3, 3, three_by_two);
    assert(framebuffer[2 * ROW_BYTES] == 0x50);
    assert(framebuffer[3 * ROW_BYTES] == 0x20);

    memset(framebuffer, 0, sizeof(framebuffer));
    const uint8_t nine_by_two[] = {0xAA, 0x80, 0x55, 0x00};
    blit(framebuffer, 8, 4, 16, 5, nine_by_two);
    assert(framebuffer[4 * ROW_BYTES + 1] == 0xAA);
    assert(framebuffer[4 * ROW_BYTES + 2] == 0x80);
    assert(framebuffer[5 * ROW_BYTES + 1] == 0x55);
    assert(framebuffer[5 * ROW_BYTES + 2] == 0x00);

    memset(framebuffer, 0, sizeof(framebuffer));
    uint8_t full_width[ROW_BYTES * 2];
    memset(full_width, 0xA5, ROW_BYTES);
    memset(full_width + ROW_BYTES, 0x5A, ROW_BYTES);
    blit(framebuffer, 0, 7, WIDTH - 1, 8, full_width);
    assert(memcmp(framebuffer + 7 * ROW_BYTES, full_width, sizeof(full_width)) == 0);

    memset(framebuffer, 0, sizeof(framebuffer));
    const uint8_t asymmetric[] = {0xA0}; /* logical pixels 101 at x=0..2 */
    epaper_blit_packed_rect_flipped(framebuffer, WIDTH, HEIGHT,
                                    0, 0, 2, 0, asymmetric, false, true);
    /* Columns remain left-to-right while logical row 0 maps to the last row. */
    assert(framebuffer[(HEIGHT - 1) * ROW_BYTES] == 0xA0);

    memset(framebuffer, 0, sizeof(framebuffer));
    epaper_blit_packed_rect_flipped(framebuffer, WIDTH, HEIGHT,
                                    0, 7, WIDTH - 1, 8, full_width, false, true);
    for (size_t i = 0; i < ROW_BYTES; i++) {
        assert(framebuffer[2 * ROW_BYTES + i] == 0xA5);
        assert(framebuffer[1 * ROW_BYTES + i] == 0x5A);
    }

    /* Mirroring x sends logical column 0 to the last column, and leaves rows
     * alone. 101 at x=0..2 becomes 101 at the right-hand edge: the last three
     * columns of the row, which is the top bit of the last byte set, then
     * clear, then set — 0xA0 reversed within its three columns is 0b101, so
     * the final byte reads 0b00000101 = 0x05. */
    memset(framebuffer, 0, sizeof(framebuffer));
    epaper_blit_packed_rect_flipped(framebuffer, WIDTH, HEIGHT,
                                    0, 0, 2, 0, asymmetric, true, false);
    assert(framebuffer[ROW_BYTES - 1] == 0x05);
    assert(framebuffer[0] == 0x00);

    /* Mirroring both axes is a 180-degree turn: the same three pixels land in
     * the last row as well as the last columns. */
    memset(framebuffer, 0, sizeof(framebuffer));
    epaper_blit_packed_rect_flipped(framebuffer, WIDTH, HEIGHT,
                                    0, 0, 2, 0, asymmetric, true, true);
    assert(framebuffer[HEIGHT * ROW_BYTES - 1] == 0x05);

    /* A byte-aligned full-width row mirrored in x is its bytes reversed, each
     * byte's bits reversed too. 0xA5 is 1010_0101, a palindrome under bit
     * reversal, so a row of 0xA5 mirrors to a row of 0xA5 — which makes the
     * neighbouring 0x5A row the one that proves the bytes moved. */
    memset(framebuffer, 0, sizeof(framebuffer));
    epaper_blit_packed_rect_flipped(framebuffer, WIDTH, HEIGHT,
                                    0, 7, WIDTH - 1, 8, full_width, true, false);
    for (size_t i = 0; i < ROW_BYTES; i++) {
        assert(framebuffer[7 * ROW_BYTES + i] == 0xA5);
        assert(framebuffer[8 * ROW_BYTES + i] == 0x5A);
    }

    return 0;
}
