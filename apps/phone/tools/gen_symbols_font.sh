#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Regenerates the Phone app's fonts cut from DejaVu, each antialiased for
# colour screens and one bit a pixel (_mono) for e-paper:
#
#   phone/phone_symbols_16*.c  the marks its lists set after the Reader's
#                              DejaVu 16 px text: the stars, filled and
#                              hollow, and the arrows of a call made (↗) and
#                              received (↙). Neither the Reader's font nor
#                              LVGL's Montserrat has them.
#   phone/phone_tab_12*.c      the tab bar's words, in DejaVu Sans Condensed
#                              12 px: "Favourites" in Montserrat 14 is wider
#                              than a quarter of a 240 px screen.
#
# The tab bar's pictures are drawn by gen_tab_icons.py.
#
#     apps/phone/tools/gen_symbols_font.sh [DejaVu font directory]
#
# Needs Node.js for npx.
set -eu

dir=${1:-/usr/share/fonts/truetype/dejavu}
cd "$(dirname "$0")/../phone"

gen() {  # name bpp size font, then lv_font_conv's range or symbol options
    name=$1 bpp=$2 size=$3 font=$4
    shift 4
    npx -y lv_font_conv@1.5.3 \
        --no-compress --no-prefilter --force-fast-kern-format \
        --bpp "$bpp" --size "$size" --format lvgl \
        --lv-include lvgl.h --lv-font-name "$name" \
        --font "$dir/$font" "$@" \
        -o "$name.c"
    echo "wrote $(pwd)/$name.c"
}

# ★ ☆ ↗ ↙
gen phone_symbols_16      4 16 DejaVuSans.ttf -r 0x2605,0x2606,0x2197,0x2199
gen phone_symbols_16_mono 1 16 DejaVuSans.ttf -r 0x2605,0x2606,0x2197,0x2199

tab_words="Favourites Recents Contacts Keypad"
gen phone_tab_12      4 12 DejaVuSansCondensed.ttf --symbols "$tab_words"
gen phone_tab_12_mono 1 12 DejaVuSansCondensed.ttf --symbols "$tab_words"
