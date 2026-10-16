#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Regenerates the Arabic fonts every label and text field falls back to when
# its own font has no Arabic (components/ui/src/arabic_text.c):
# src/fonts/arabic_font_{14,18,22}.c, antialiased for colour screens, and
# src/fonts/arabic_font_{14,18,22}_mono.c, one bit a pixel for e-paper.
#
#     components/ui/tools/gen_arabic_fonts.sh [DejaVuSans.ttf]
#
# DejaVu Sans, as the Reader's font and LVGL's own Arabic font are made from,
# at the sizes of the three Montserrat fonts the UI uses: the Arabic block,
# and the presentation forms arabic_shape.c writes joined letters as (Forms-A
# for the Persian letters, Forms-B for the rest). Latin is Montserrat's. The
# options are those of apps/reader/tools/gen_reader_font.sh. Needs Node.js
# for npx.
#
# A panel that shows only black or white keeps an antialiased pixel only if it
# is at least half ink, and the dots of Arabic letters, spread over a few
# lighter pixels, lose some of theirs. The mono fonts have every pixel of a
# letter either inked or not to begin with, so their dots come out whole.
set -eu

ttf=${1:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}
cd "$(dirname "$0")/../src/fonts"

gen() {  # name size bpp
    npx -y lv_font_conv@1.5.3 \
        --no-compress --no-prefilter --force-fast-kern-format \
        --bpp "$3" --size "$2" --format lvgl \
        --lv-include lvgl.h --lv-font-name "$1" \
        --font "$ttf" \
        -r 0x600-0x6FF,0xFB50-0xFBFF,0xFE70-0xFEFF \
        -o "$1.c"
    echo "wrote $(pwd)/$1.c"
}

for size in 14 18 22; do
    gen "arabic_font_$size" "$size" 4
    gen "arabic_font_${size}_mono" "$size" 1
done
