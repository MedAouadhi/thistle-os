# SPDX-License-Identifier: BSD-3-Clause
# ThistleOS — libopus, for the firmware (CMakeLists.txt here) and the
# simulators (simulator/CMakeLists.txt), so that both build the same code.
#
# libopus 1.5.2 (BSD 3-clause, opus-1.5.2/COPYING) is vendored from the
# release tarball, https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz,
# SHA-256 65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1,
# with what neither build compiles left out: the DNN (deep PLC, DRED, OSCE,
# 18 MB of weights), the float SILK, the ARM, x86 and MIPS code, the tests and
# demos. The source lists are upstream's own *_sources.mk, read here, so an
# upgrade is a new directory and THISTLE_OPUS_VERSION.
#
# Fixed point, which an S3 runs far faster than float, and no float API.
# The pseudostack (NONTHREADSAFE_PSEUDOSTACK) is one scratch buffer that every
# encoder and decoder shares: run them from one task at a time. It and the
# codec states are allocated by thistle_opus.c, from PSRAM on the device
# (custom_support.h).

set(THISTLE_OPUS_VERSION 1.5.2)
set(THISTLE_OPUS_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(THISTLE_OPUS_UPSTREAM ${THISTLE_OPUS_ROOT}/opus-${THISTLE_OPUS_VERSION})

# The files a make variable lists, NAME = \ a.c \ b.c, as absolute paths.
function(thistle_opus_mk_list out mkfile var)
    file(READ ${mkfile} text)
    string(REGEX REPLACE "\\\\\n" " " text "${text}")
    string(REGEX MATCH "(^|\n)${var} =([^\n]*)" line "${text}")
    string(REGEX REPLACE "(^|\n)${var} =" "" line "${line}")
    separate_arguments(files UNIX_COMMAND "${line}")
    set(paths)
    foreach(f IN LISTS files)
        list(APPEND paths ${THISTLE_OPUS_UPSTREAM}/${f})
    endforeach()
    set(${out} ${paths} PARENT_SCOPE)
endfunction()

thistle_opus_mk_list(_celt ${THISTLE_OPUS_UPSTREAM}/celt_sources.mk CELT_SOURCES)
thistle_opus_mk_list(_silk ${THISTLE_OPUS_UPSTREAM}/silk_sources.mk SILK_SOURCES)
thistle_opus_mk_list(_silk_fixed ${THISTLE_OPUS_UPSTREAM}/silk_sources.mk SILK_SOURCES_FIXED)
thistle_opus_mk_list(_opus ${THISTLE_OPUS_UPSTREAM}/opus_sources.mk OPUS_SOURCES)

set(THISTLE_OPUS_SRCS ${_celt} ${_silk} ${_silk_fixed} ${_opus}
    ${THISTLE_OPUS_ROOT}/src/thistle_opus.c)

# What a user of the component includes: opus.h and thistle_opus.h
set(THISTLE_OPUS_PUBLIC_INCLUDES
    ${THISTLE_OPUS_UPSTREAM}/include
    ${THISTLE_OPUS_ROOT}/include)

set(THISTLE_OPUS_PRIVATE_INCLUDES
    ${THISTLE_OPUS_ROOT}/src
    ${THISTLE_OPUS_UPSTREAM}
    ${THISTLE_OPUS_UPSTREAM}/celt
    ${THISTLE_OPUS_UPSTREAM}/silk
    ${THISTLE_OPUS_UPSTREAM}/silk/fixed)

set(THISTLE_OPUS_DEFINITIONS
    OPUS_BUILD
    FIXED_POINT=1
    DISABLE_FLOAT_API
    NONTHREADSAFE_PSEUDOSTACK
    # The pseudostack's size: an encoder and a decoder at 16 kHz used
    # at most 14076 B of it on the S3, 14816 B on a 64-bit host, at
    # complexity from 9 down to 0 (thistle_opus_scratch_used();
    # .scratch/tailnet-chat/issues/01-hardware-spike.md). libopus does not
    # check it: running past it writes over the heap.
    GLOBAL_STACK_SIZE=20480
    CUSTOM_SUPPORT
    HAVE_LRINT
    HAVE_LRINTF
    PACKAGE_VERSION="${THISTLE_OPUS_VERSION}")

set(THISTLE_OPUS_WARNINGS
    -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function
    -Wno-unused-variable -Wno-maybe-uninitialized -Wno-stringop-overflow)
