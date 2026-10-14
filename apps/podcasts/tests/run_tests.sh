#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Builds the Podcasts app's code that has no hardware or LVGL in it for the
# host, under AddressSanitizer and UndefinedBehaviorSanitizer, and runs its
# tests.
#
#     apps/podcasts/tests/run_tests.sh
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
    for src in "$@"; do srcs="$srcs $app/podcasts/$src"; done
    # shellcheck disable=SC2086
    $cc $flags $srcs "$here/$name.c" -o "$work/$name"
}

build test_pod_sha1 pod_sha1.c
build test_pod_json pod_json.c
build test_pod_text pod_text.c
build test_pod_api pod_api.c pod_json.c pod_sha1.c pod_text.c
build test_pod_library pod_library.c pod_json.c pod_text.c

"$work/test_pod_sha1"
"$work/test_pod_json"
"$work/test_pod_text"
"$work/test_pod_api"
POD_TEST_DIR="$work" "$work/test_pod_library"
