#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Regenerates the Podcasts app's symbol font: podcasts/pod_symbols_16.c,
# antialiased for colour screens, and podcasts/pod_symbols_16_mono.c, one bit
# a pixel for e-paper.
#
#     apps/podcasts/tools/gen_symbols_font.sh [DejaVuSans.ttf]
#
# The two stars, filled and hollow, from DejaVu Sans at 16 px, the font and
# size the Reader's text font is, which the app sets its lists in: neither
# that font nor LVGL's Montserrat has them. The app chains the fonts, text
# then these then Montserrat for LVGL's symbols. Needs Node.js for npx.
set -eu

ttf=${1:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}
cd "$(dirname "$0")/../podcasts"

gen() {  # name bpp
    npx -y lv_font_conv@1.5.3 \
        --no-compress --no-prefilter --force-fast-kern-format \
        --bpp "$2" --size 16 --format lvgl \
        --lv-include lvgl.h --lv-font-name "$1" \
        --font "$ttf" \
        -r 0x2605,0x2606 \
        -o "$1.c"
    echo "wrote $(pwd)/$1.c"
}

gen pod_symbols_16 4
gen pod_symbols_16_mono 1
