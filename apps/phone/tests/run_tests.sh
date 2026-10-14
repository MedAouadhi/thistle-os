#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Builds the Phone app's code that has no hardware or LVGL in it for the
# host, under AddressSanitizer and UndefinedBehaviorSanitizer, and runs its
# tests.
#
#     apps/phone/tests/run_tests.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$here")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc=${CC:-cc}
flags="-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g -I$app"

# shellcheck disable=SC2086
$cc $flags "$app/phone/phone_text.c" "$here/test_phone_text.c" -o "$work/test_phone_text"

TZ=UTC "$work/test_phone_text"
