#!/bin/sh
# The firmware's version is its tag: prints what `git describe` says of the
# repository (the first argument, else this script's), matching firmware tags
# only, so an app's `notes-v1.0.1` never becomes the firmware's version, with
# the leading `v` dropped: `0.6.0`, `0.6.0-rc.1`, `0.5.2-108-gabc1234-dirty`.
# With no firmware tag in reach, `0.0.0-unknown`.
#
# Used by the top-level CMakeLists.txt (ESP-IDF's PROJECT_VER, which the
# kernel reports), components/kernel_rs/build.rs (the simulator and host
# tests) and the firmware release workflow. Tested by test_firmware_version.py.

repo=${1:-$(dirname "$0")/..}
described=$(git -C "$repo" describe --tags --match 'v[0-9]*' --dirty 2>/dev/null) || described=
case "$described" in
    v[0-9]*) echo "${described#v}" ;;
    *) echo "0.0.0-unknown" ;;
esac
