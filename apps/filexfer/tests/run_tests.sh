#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Builds the File Transfer app's code that has no hardware, server or LVGL in
# it for the host, under AddressSanitizer and UndefinedBehaviorSanitizer, and
# runs its tests.
#
#     apps/filexfer/tests/run_tests.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$here")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc=${CC:-cc}
flags="-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g -I$app"

build() {  # test name, then the app's sources it needs
    name=$1
    shift
    srcs=""
    for src in "$@"; do srcs="$srcs $app/filexfer/$src"; done
    # shellcheck disable=SC2086
    $cc $flags $srcs "$here/$name.c" -o "$work/$name"
}

build test_xfer_path xfer_path.c
build test_xfer_auth xfer_auth.c
build test_xfer_fs xfer_fs.c xfer_path.c

"$work/test_xfer_path"
"$work/test_xfer_auth"
mkdir "$work/fs"
XFER_TEST_DIR="$work/fs" "$work/test_xfer_fs"
