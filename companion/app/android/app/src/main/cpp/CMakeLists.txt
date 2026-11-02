# SPDX-License-Identifier: BSD-3-Clause
# Thistle Chat on Android: libopus.so for each ABI, which the core opens by
# FFI (companion/core/lib/src/opus.dart). Built from the libopus the
# firmware and the simulators build (components/opus/opus.cmake, libopus
# 1.5.2 vendored), with the same source lists and fixed point, but none of
# the firmware's allocation: the codec states come from malloc, and its
# scratch from the stack (VAR_ARRAYS), as the app runs an encoder and a
# decoder in more than one thread.
cmake_minimum_required(VERSION 3.22)
project(thistle_chat_opus C)

set(THISTLE_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../../../../..)
include(${THISTLE_ROOT}/components/opus/opus.cmake)

set(srcs ${THISTLE_OPUS_SRCS})
list(REMOVE_ITEM srcs ${THISTLE_OPUS_ROOT}/src/thistle_opus.c)

add_library(opus SHARED ${srcs})
target_include_directories(opus PRIVATE
    ${THISTLE_OPUS_UPSTREAM}/include
    ${THISTLE_OPUS_UPSTREAM}
    ${THISTLE_OPUS_UPSTREAM}/celt
    ${THISTLE_OPUS_UPSTREAM}/silk
    ${THISTLE_OPUS_UPSTREAM}/silk/fixed)
target_compile_definitions(opus PRIVATE
    OPUS_BUILD
    FIXED_POINT=1
    DISABLE_FLOAT_API
    VAR_ARRAYS
    HAVE_LRINT
    HAVE_LRINTF
    PACKAGE_VERSION="${THISTLE_OPUS_VERSION}")
# Only libopus' API (OPUS_EXPORT) is exported.
target_compile_options(opus PRIVATE -O2 -fvisibility=hidden
    -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function -Wno-unused-variable)
target_link_libraries(opus PRIVATE m)
