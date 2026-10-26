# ThistleOS Rust kernel component (REQUIRED)

set(KERNEL_RS_REQUIRES
    thistle_hal esp_timer freertos esp_partition fatfs vfs app_update
    esp_wifi esp_netif esp_event mbedtls esp_http_client driver spiffs
    nvs_flash esp_adc sdmmc elf_loader bt
)

idf_component_register(
    SRCS "stub.c" "epaper_spi_shim.c" "http_client_shim.c"
    INCLUDE_DIRS "include"
    REQUIRES ${KERNEL_RS_REQUIRES}
    WHOLE_ARCHIVE
)

set(RUST_TARGET_FEATURES "")
if(IDF_TARGET STREQUAL "esp32")
    set(RUST_TARGET "xtensa-esp32-espidf")
elseif(IDF_TARGET STREQUAL "esp32s2")
    set(RUST_TARGET "xtensa-esp32s2-espidf")
elseif(IDF_TARGET STREQUAL "esp32s3")
    set(RUST_TARGET "xtensa-esp32s3-espidf")
elseif(IDF_TARGET STREQUAL "esp32c3" OR IDF_TARGET STREQUAL "esp32c6")
    set(RUST_TARGET "riscv32imc-esp-espidf")
    # ESP-IDF links these chips as rv32imac. The generic Rust ESP target omits
    # `a`, which otherwise leaves unresolved __atomic_* runtime calls; emit the
    # hardware atomic instructions that the selected chips support.
    set(RUST_TARGET_FEATURES "-C target-feature=+a")
else()
    message(FATAL_ERROR "Unsupported ThistleOS target: ${IDF_TARGET}")
endif()

set(RUST_FEATURES "${IDF_TARGET}")
if(CONFIG_BT_ENABLED)
    string(APPEND RUST_FEATURES ",ble-hardware")
endif()
set(RUST_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/target/${RUST_TARGET}/release")
set(RUST_LIB "${RUST_BUILD_DIR}/libthistle_kernel.a")

# The public half of the build machine's app publisher key, if
# tools/make_card_appstore.py has made one: the kernel trusts apps signed with
# it (kernel_boot.rs). Read when cargo runs, not when CMake configures, so
# making the key needs no reconfigure; cargo rebuilds when it changes. The key
# pair is the machine's own and git ignores both halves.
set(APP_PUBLISHER_KEY "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/local_publisher_public.key")

# The AssistNow token, ASSISTNOW_ZTP_TOKEN in the repository's .env (which git
# ignores): the token of a device profile in the user's Thingstream account,
# which registers the GPS receiver for u-blox's free orbit predictions
# (gps_assist.rs). Read when cargo runs, as the key above, and never printed;
# a card's config/assistnow.json overrides it. The simulator's build has none,
# so a simulated receiver is never registered with Thingstream.
set(REPO_ENV "${CMAKE_CURRENT_SOURCE_DIR}/../../.env")

# The firmware's version, as the top-level CMakeLists.txt described it: the
# kernel reports exactly what the image's description holds (version.rs).
idf_build_get_property(THISTLE_FW_VERSION PROJECT_VER)

# Write a build script that sources the ESP Rust env in isolation
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/build_rust.sh"
"#!/bin/bash\nexport PATH=\"$ENV{HOME}/.cargo/bin:$PATH\"\nif [ -f \"$ENV{HOME}/export-esp.sh\" ]; then . \"$ENV{HOME}/export-esp.sh\" 2>/dev/null; fi\ncd \"${CMAKE_CURRENT_SOURCE_DIR}\"\nexport THISTLE_FW_VERSION=\"${THISTLE_FW_VERSION}\"\nexport RUSTFLAGS=\"${RUST_TARGET_FEATURES}\"\nif [ -f \"${APP_PUBLISHER_KEY}\" ]; then export THISTLE_APP_PUBLISHER_KEY=$(od -An -v -tx1 \"${APP_PUBLISHER_KEY}\" | tr -d ' \\n'); else unset THISTLE_APP_PUBLISHER_KEY; fi\nif [ -f \"${REPO_ENV}\" ]; then export THISTLE_ASSISTNOW_TOKEN=$(sed -n -E 's/^[[:space:]]*(export[[:space:]]+)?ASSISTNOW_ZTP_TOKEN[[:space:]]*=[[:space:]]*\"?([^\"]*)\"?[[:space:]]*$/\\2/p' \"${REPO_ENV}\" | tail -n 1); else unset THISTLE_ASSISTNOW_TOKEN; fi\ncargo +esp build --release --target ${RUST_TARGET} --features ${RUST_FEATURES} -Zbuild-std=std,panic_abort\n")

# Cargo runs on every build and decides for itself what is out of date. It
# tracks every input, including thistle-tk under crates/, which the glob of
# src/*.rs this replaced never saw, so an edit there never reached the
# firmware. With nothing to do cargo takes a tenth of a second and leaves the
# library's timestamp alone, and because the library is a BYPRODUCT, Ninja
# restats it and nothing downstream reruns.
add_custom_target(kernel_rs_build
    COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/build_rust.sh"
    BYPRODUCTS ${RUST_LIB}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building Rust kernel (thistle-kernel)"
)
add_dependencies(${COMPONENT_LIB} kernel_rs_build)

# A new Rust library has to rebuild this component's archive, and with it
# the merge below. Making stub.c's object depend on the library does that
# in the same build. This used to touch stub.c after cargo ran instead, but
# Ninja reads timestamps once, when it starts, so the touch counted only on
# the next build: the first build after a Rust change linked the old Rust
# code into the image.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/stub.c"
    PROPERTIES OBJECT_DEPENDS ${RUST_LIB})

# Merge Rust staticlib objects into this component's .a archive so the
# linker sees them as part of __idf_kernel_rs (inside ESP-IDF's
# --start-group block). This avoids the INTERFACE/PUBLIC link propagation
# placing the Rust lib outside the group.
add_custom_command(TARGET ${COMPONENT_LIB} POST_BUILD
    COMMAND ${CMAKE_AR} x ${RUST_LIB}
    COMMAND ${CMAKE_AR} rcs $<TARGET_FILE:${COMPONENT_LIB}> *.o
    COMMAND ${CMAKE_COMMAND} -E remove -f *.o
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Merging Rust kernel objects into ${COMPONENT_LIB}"
)
