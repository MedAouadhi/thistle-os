#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Builds the Reader's EPUB, reading progress and book title code for the host
# under AddressSanitizer and UndefinedBehaviorSanitizer, and runs their tests,
# the EPUB ones against fresh fixtures. Its Arabic shaping is the system's,
# components/ui/src/arabic_shape.c, tested with the simulator's unit tests.
#
#     apps/reader/tests/run_tests.sh
#
# The vendored Expat and uzlib are built without -Werror: they are upstream's
# code, kept as upstream wrote it.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$here")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc=${CC:-cc}
sanitize="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
includes="-I$app"

for src in expat/xmlparse.c expat/xmlrole.c expat/xmltok.c \
           uzlib/tinflate.c uzlib/adler32.c uzlib/crc32.c; do
    $cc -std=c11 $sanitize $includes -w -c "$app/third_party/$src" \
        -o "$work/$(basename "$src" .c).o"
done

$cc -std=c11 -Wall -Wextra -Werror $sanitize $includes \
    "$app/reader/epub.c" "$app/reader/epub_zip.c" "$app/reader/html_entities.c" \
    "$here/test_epub.c" "$work"/*.o -o "$work/test_epub"

$cc -std=c11 -Wall -Wextra -Werror $sanitize $includes \
    "$app/reader/book_progress.c" "$here/test_book_progress.c" "$work/crc32.o" \
    -o "$work/test_book_progress"

$cc -std=c11 -Wall -Wextra -Werror $sanitize $includes \
    "$app/reader/book_title.c" "$here/test_book_title.c" -o "$work/test_book_title"

python3 "$here/make_fixtures.py" "$work/fixtures"
"$work/test_epub" "$work/fixtures"
"$work/test_book_progress" "$work/card"
"$work/test_book_title"
