#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Builds the map engine's Pack reader and view composition, the parts with
# no LVGL or task in them, for the host under AddressSanitizer and
# UndefinedBehaviorSanitizer, and runs their tests over the Map Pack tool's
# golden fixture and hand-damaged copies of it.
#
#     components/map_engine/tests/run_tests.sh
#
# The host has no ROM inflater: it inflates with the Reader's uzlib, as the
# simulator does, built without -Werror as upstream's code.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
engine=$(dirname "$here")
root=$(cd "$engine/../.." && pwd)
uzlib="$root/apps/reader/third_party/uzlib"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc=${CC:-cc}
sanitize="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g"
includes="-I$engine/include -I$engine/src -I$here/host -I$uzlib"

for src in tinflate adler32 crc32; do
    $cc -std=c11 $sanitize $includes -w -c "$uzlib/$src.c" -o "$work/$src.o"
done

$cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror $sanitize $includes \
    "$engine/src/map_pack.c" "$engine/src/map_inflate.c" "$engine/src/map_compose.c" \
    "$here/test_map_pack.c" "$work/tinflate.o" "$work/adler32.o" "$work/crc32.o" -lm -o "$work/test_map_pack"

"$work/test_map_pack" "$root/tools/maps/testdata/fixture.tmap" "$work"
