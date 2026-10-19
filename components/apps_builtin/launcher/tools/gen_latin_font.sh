#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Regenerates the fallback for the detail line of Home's rows of what is new
# (launcher_ui.c): fonts/home_latin_14.c, antialiased for colour screens, and
# fonts/home_latin_14_mono.c, one bit a pixel for e-paper.
#
#     components/apps_builtin/launcher/tools/gen_latin_font.sh [Montserrat-Medium.ttf]
#
# LVGL's Montserrat 14 has ASCII alone, and a missed call is named from the
# phonebook: "Ben Müller" drew a box for its "ü". This is the same face and
# size with Latin-1 and Latin Extended-A, the Latin the Reader's font has,
# and nothing that Montserrat 14 has already. Needs Node.js for npx.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
ttf=${1:-$here/../../../managed_components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf}
cd "$here/fonts"

gen() {  # name bpp
    npx -y lv_font_conv@1.5.3 \
        --no-compress --no-prefilter --force-fast-kern-format \
        --bpp "$2" --size 14 --format lvgl \
        --lv-include lvgl.h --lv-font-name "$1" \
        --font "$ttf" \
        -r 0xA0-0x17F \
        -o "$1.c"
    echo "wrote $(pwd)/$1.c"
}

gen home_latin_14 4
gen home_latin_14_mono 1
