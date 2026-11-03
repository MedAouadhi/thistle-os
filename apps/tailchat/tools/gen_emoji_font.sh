#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Regenerates the Chat app's emoji, cut from Noto Emoji, Google's emoji of
# one colour (SIL Open Font License 1.1), each antialiased for colour
# screens and one bit a pixel (_mono) for e-paper:
#
#   tailchat/tailchat_emoji_14*.c  in messages and reactions, after
#                                  Montserrat 14 and the app's pictures
#   tailchat/tailchat_emoji_20*.c  the reactions offered, alone, on the
#                                  buttons of the message sheet
#
# The emoji most sent, about a hundred and ten of them, not all of Unicode's:
# a font of every emoji would be some 150 KB of a kernel slot. One not here
# is LVGL's empty box. The heart is the app's own picture
# (gen_icons.py): Noto's is hatched, which one bit a pixel makes stripes.
#
#     apps/tailchat/tools/gen_emoji_font.sh [NotoEmoji[wght].ttf]
#
# Without a font file it fetches the one pinned below and checks it. Needs
# Node.js for npx, and curl.
set -eu

url="https://raw.githubusercontent.com/google/fonts/2fe1e8c6b137fb13041a7cfca6fdcd8a910ea4dc/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf"
sha256="de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551"

ttf=${1:-}
if [ -z "$ttf" ]; then
    ttf=$(mktemp -d)/NotoEmoji.ttf
    curl -sSfL -o "$ttf" "$url"
fi
echo "$sha256  $ttf" | sha256sum -c - >/dev/null || {
    echo "$ttf is not the Noto Emoji this was made from ($sha256)" >&2
    exit 1
}
cd "$(dirname "$0")/../tailchat"

# Faces, then hands and people, hearts, then things and signs.
emoji="0x1F600-0x1F60F,0x1F610-0x1F612,0x1F614-0x1F618,0x1F61A,0x1F61C-0x1F61E,0x1F620-0x1F625,0x1F629-0x1F62D,"
emoji="${emoji}0x1F62E,0x1F630,0x1F631,0x1F633,0x1F634,0x1F637,0x1F641,0x1F642,0x1F643,0x1F644,"
emoji="${emoji}0x1F910,0x1F914,0x1F917,0x1F923,0x1F924,0x1F929,0x1F92A,0x1F92D,0x1F92F,0x1F970,0x1F972,0x1F973,"
emoji="${emoji}0x1F97A,0x2639,0x263A,"
emoji="${emoji}0x1F44B,0x1F44C,0x1F44D,0x1F44E,0x1F44F,0x1F450,0x1F64C,0x1F64F,0x1F4AA,0x1F91D,0x1F91E,0x270C,"
emoji="${emoji}0x261D,0x1F449,0x1F448,0x1F446,0x1F447,0x1F440,0x1F926,0x1F937,0x1F648,"
emoji="${emoji}0x1F493,0x1F494,0x1F495,0x1F496,0x1F497,0x1F498,0x1F499,0x1F49A,0x1F49B,0x1F49C,0x1F5A4,0x1F9E1,"
emoji="${emoji}0x2763,"
emoji="${emoji}0x1F525,0x2728,0x2B50,0x1F31F,0x1F389,0x1F388,0x1F381,0x1F382,0x1F4AF,0x2705,0x274C,0x2753,0x2757,"
emoji="${emoji}0x26A0,0x1F44A,0x1F3B6,0x1F3B5,0x2615,0x1F37A,0x1F37B,0x1F377,0x1F355,0x1F354,0x1F370,0x1F338,"
emoji="${emoji}0x1F339,0x1F490,0x1F31E,0x2600,0x1F319,0x2614,0x2744,0x1F3E0,0x1F697,0x1F68C,0x2708,0x1F4F7,"
emoji="${emoji}0x1F4F1,0x1F4BB,0x1F4A1,0x1F4DE,0x1F4CD,0x23F0,0x1F680,0x1F4B0,0x1F6A8,0x1F48B,0x1F4A9,0x1F480,"
emoji="${emoji}0x1F47B,0x1F916,0x1F436,0x1F431"

# tailchat_reactions in tailchat_core.c, the heart aside.
reactions="0x1F44D,0x1F602,0x1F62E,0x1F622,0x1F64F"

gen() {  # name bpp size ranges
    npx -y lv_font_conv@1.5.3 \
        --no-compress --no-prefilter --force-fast-kern-format \
        --bpp "$2" --size "$3" --format lvgl \
        --lv-include lvgl.h --lv-font-name "$1" \
        --font "$ttf" -r "$4" \
        -o "$1.c"
    echo "wrote $(pwd)/$1.c"
}

gen tailchat_emoji_14 4 14 "$emoji"
gen tailchat_emoji_14_mono 1 14 "$emoji"
gen tailchat_emoji_20 4 20 "$reactions"
gen tailchat_emoji_20_mono 1 20 "$reactions"
