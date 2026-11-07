#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Builds the chat apps' conversation store, which has no LVGL in it, for the
# host, under AddressSanitizer and UndefinedBehaviorSanitizer, and runs its
# tests.
#
#     apps/chatkit/tests/run_tests.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
kit=$(dirname "$here")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc=${CC:-cc}
flags="-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g -I$kit"

# shellcheck disable=SC2086
$cc $flags "$kit/chatkit/chatkit_store.c" "$here/test_chatkit_store.c" -o "$work/test_chatkit_store"
"$work/test_chatkit_store"
