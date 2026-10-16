#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Builds the Hermes app's code that has no hardware or LVGL in it for the
# host, under AddressSanitizer and UndefinedBehaviorSanitizer, and runs its
# tests.
#
#     apps/hermes/tests/run_tests.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$here")
apps=$(dirname "$app")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc=${CC:-cc}
# The Podcasts app's JSON reader and SHA-1 are shared, as "podcasts/...".
flags="-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g -I$app -I$apps/podcasts"

build() {  # test name, then the sources it needs, relative to apps/
    name=$1
    shift
    srcs=""
    for src in "$@"; do srcs="$srcs $apps/$src"; done
    # shellcheck disable=SC2086
    $cc $flags $srcs "$here/$name.c" -o "$work/$name"
}

build test_hermes_ws hermes/hermes/hermes_ws.c podcasts/podcasts/pod_sha1.c
build test_hermes_proto hermes/hermes/hermes_proto.c hermes/hermes/hermes_chat.c \
    podcasts/podcasts/pod_json.c
build test_hermes_glyphs hermes/hermes/hermes_glyphs.c

"$work/test_hermes_ws"
"$work/test_hermes_proto"
"$work/test_hermes_glyphs"
