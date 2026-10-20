#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Builds the To-do app's list and file format, which have no LVGL or files in
# them, for the host, under AddressSanitizer and UndefinedBehaviorSanitizer,
# and runs their tests.
#
#     apps/todo/tests/run_tests.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$here")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc=${CC:-cc}
flags="-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g -I$app"

# shellcheck disable=SC2086
$cc $flags "$app/todo/todo_core.c" "$here/test_todo_core.c" -o "$work/test_todo_core"
"$work/test_todo_core"
