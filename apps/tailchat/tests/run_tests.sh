#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Builds the Chat app's text, which has no LVGL in it, for the host, under
# AddressSanitizer and UndefinedBehaviorSanitizer, and runs its tests.
#
#     apps/tailchat/tests/run_tests.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$here")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc=${CC:-cc}
flags="-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g -I$app"

# shellcheck disable=SC2086
$cc $flags "$app/tailchat/tailchat_core.c" "$here/test_tailchat_core.c" -o "$work/test_tailchat_core"
"$work/test_tailchat_core"
