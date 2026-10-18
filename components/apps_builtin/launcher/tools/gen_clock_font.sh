#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Regenerates the Home screen's clock font (launcher_ui.c):
# fonts/home_clock_44.c, antialiased for colour screens, and
# fonts/home_clock_44_mono.c, one bit a pixel for e-paper.
#
#     components/apps_builtin/launcher/tools/gen_clock_font.sh [Montserrat-Medium.ttf]
#
# Montserrat Medium, as LVGL's built-in fonts are made from, at 44 px. Only
# what time_manager_format_time() writes: the digits, the colon, the dashes of
# "--:--" before the clock is set, and the space and letters of "2:05 PM".
# The options are those of components/ui/tools/gen_arabic_fonts.sh. Needs
# Node.js for npx.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
ttf=${1:-$here/../../../managed_components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf}
cd "$here/fonts"

gen() {  # name bpp
    npx -y lv_font_conv@1.5.3 \
        --no-compress --no-prefilter --force-fast-kern-format \
        --bpp "$2" --size 44 --format lvgl \
        --lv-include lvgl.h --lv-font-name "$1" \
        --font "$ttf" \
        -r 0x20,0x2D,0x30-0x3A,0x41,0x4D,0x50 \
        -o "$1.c"
    echo "wrote $(pwd)/$1.c"
}

gen home_clock_44 4
gen home_clock_44_mono 1
