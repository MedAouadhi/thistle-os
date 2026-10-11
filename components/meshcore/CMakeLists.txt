# SPDX-License-Identifier: BSD-3-Clause
# ThistleOS — MeshCore mesh protocol integration
#
# Builds the MeshCore C++ library (MIT, git submodule) as an ESP-IDF
# component, with the engine in src/meshcore_shim.cpp that runs it on the HAL
# radio. Only the core protocol files are compiled, not the examples or UI.
#
# MeshCore is written for the Arduino framework. Rather than pull in all of
# arduino-esp32, include/ stands in for the few Arduino headers it reaches for
# (Arduino.h, Stream.h, FS.h) and routes its crypto (Ed25519.h, ed_25519.h,
# SHA256.h, AES.h) to the kernel's.

set(MESHCORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/MeshCore/src)

set(MESHCORE_SRCS
    ${MESHCORE_DIR}/Dispatcher.cpp
    ${MESHCORE_DIR}/Identity.cpp
    ${MESHCORE_DIR}/Mesh.cpp
    ${MESHCORE_DIR}/Packet.cpp
    ${MESHCORE_DIR}/Utils.cpp
    ${MESHCORE_DIR}/helpers/BaseChatMesh.cpp
    ${MESHCORE_DIR}/helpers/AdvertDataHelpers.cpp
    ${MESHCORE_DIR}/helpers/TxtDataHelpers.cpp
    # IdentityStore.cpp and TransportKeyStore.cpp are left out: they use the
    # Arduino filesystem. The engine keeps its identity and contacts itself.
    ${MESHCORE_DIR}/helpers/StaticPoolPacketManager.cpp
    src/meshcore_shim.cpp
)

idf_component_register(
    SRCS ${MESHCORE_SRCS}
    INCLUDE_DIRS
        "include"
        "${MESHCORE_DIR}"
        "${MESHCORE_DIR}/helpers"
    REQUIRES thistle_hal kernel driver esp_driver_i2c esp_driver_spi esp_driver_gpio esp_timer
)

# BaseChatMesh's size depends on these, so every file that includes it has to
# see the same values. MAX_GROUP_CHANNELS also switches on group channels at
# all: without it there is no Public channel.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MAX_CONTACTS=64
    MAX_GROUP_CHANNELS=4
)

# Suppress warnings in third-party MeshCore code
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare -Wno-reorder
    -Wno-missing-field-initializers -Wno-type-limits -Wno-format
    -Wno-delete-non-virtual-dtor -Wno-class-memaccess
)
