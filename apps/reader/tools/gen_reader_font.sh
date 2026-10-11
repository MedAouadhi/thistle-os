#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Regenerates the fonts the Reader sets books in: reader/reader_font_16.c,
# antialiased for colour screens, and reader/reader_font_16_mono.c, one bit a
# pixel for e-paper.
#
#     apps/reader/tools/gen_reader_font.sh [DejaVuSans.ttf]
#
# DejaVu Sans, as LVGL's own Arabic font is made from, at 16 px: Latin with
# Latin-1 and Latin Extended-A, the punctuation books use (dashes, curly
# quotes, ellipsis, bullet), and Arabic with both blocks of presentation
# forms, which LVGL's shaping writes joined letters as. The options are those
# of LVGL's lv_font_dejavu_16_persian_hebrew.c. Needs Node.js for npx.
#
# A panel that shows only black or white keeps an antialiased pixel only if it
# is at least half ink, and the dots of Arabic letters, spread over a few
# lighter pixels, lose some of theirs. The mono font has every pixel of a
# letter either inked or not to begin with, so its dots come out whole.
set -eu

ttf=${1:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}
cd "$(dirname "$0")/../reader"

gen() {  # name bpp
    npx -y lv_font_conv@1.5.3 \
        --no-compress --no-prefilter --force-fast-kern-format \
        --bpp "$2" --size 16 --format lvgl \
        --lv-include lvgl.h --lv-font-name "$1" \
        --font "$ttf" \
        -r 0x20-0x7E,0xA0-0x17F \
        -r 0x2010-0x2027,0x2030-0x203A,0x20AC,0x2122,0x2212 \
        -r 0x600-0x6FF,0xFB50-0xFDFF,0xFE70-0xFEFF \
        -o "$1.c"
    echo "wrote $(pwd)/$1.c"
}

gen reader_font_16 4
gen reader_font_16_mono 1
