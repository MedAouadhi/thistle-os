# SPDX-License-Identifier: BSD-3-Clause
# ThistleOS — libopus as an ESP-IDF component (opus.cmake has the sources and
# the settings, shared with the simulators).

include(${CMAKE_CURRENT_LIST_DIR}/opus.cmake)

idf_component_register(
    SRCS ${THISTLE_OPUS_SRCS}
    INCLUDE_DIRS ${THISTLE_OPUS_PUBLIC_INCLUDES}
    PRIV_INCLUDE_DIRS ${THISTLE_OPUS_PRIVATE_INCLUDES}
    PRIV_REQUIRES heap
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE ${THISTLE_OPUS_DEFINITIONS})
# libopus is speed-critical; the firmware builds for size otherwise.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 ${THISTLE_OPUS_WARNINGS})
