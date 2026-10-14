cmake_minimum_required(VERSION 3.16)
project(thistle_sim C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Find SDL2
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

# LVGL source — use the managed component downloaded by ESP-IDF build
set(LVGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/lvgl__lvgl")

# Collect LVGL sources (core + widgets + draw + display + misc)
file(GLOB_RECURSE LVGL_SRCS
    "${LVGL_DIR}/src/*.c"
)

# ThistleOS source files (kernel, UI, HAL, apps)
set(THISTLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(THISTLE_SRCS
    # HAL (registry moved to Rust — hal_registry.rs in kernel_rs)
    ${THISTLE_DIR}/components/thistle_hal/src/stub.c
    # Typed HTTP boundary used by the Rust app-store client
    ${THISTLE_DIR}/components/kernel_rs/http_client_shim.c
    ${THISTLE_DIR}/components/kernel/src/board_fingerprint.c
    # Widget API dispatch (Rust widget.rs → WM vtable bridge)
    ${THISTLE_DIR}/components/ui/src/widget_shims.c
    # thistle-tk WM C vtable + HAL bridge
    ${THISTLE_DIR}/components/kernel/src/tk_wm_shims.c
    # UI
    ${THISTLE_DIR}/components/ui/src/manager.c
    ${THISTLE_DIR}/components/ui/src/lvgl_wm.c
    ${THISTLE_DIR}/components/ui/src/lvgl_wm_epaper.c
    ${THISTLE_DIR}/components/ui/src/lvgl_wm_lcd.c
    ${THISTLE_DIR}/components/ui/src/wm_select.c
    ${THISTLE_DIR}/components/ui/src/theme.c
    ${THISTLE_DIR}/components/ui/src/statusbar.c
    ${THISTLE_DIR}/components/ui/src/app_switcher.c
    ${THISTLE_DIR}/components/ui/src/epaper_refresh.c
    ${THISTLE_DIR}/components/ui/src/toast.c
    ${THISTLE_DIR}/components/ui/src/power_ui.c
    ${THISTLE_DIR}/components/ui/src/pull_gesture.c
    ${THISTLE_DIR}/components/ui/src/quick_panel.c
    ${THISTLE_DIR}/components/ui/src/quick_panel_tiles.c
    ${THISTLE_DIR}/components/ui/src/call_ui.c
    # Built-in apps
    ${THISTLE_DIR}/components/apps_builtin/lvgl_apps.c
    ${THISTLE_DIR}/components/apps_builtin/launcher/launcher_app.c
    ${THISTLE_DIR}/components/apps_builtin/launcher/launcher_ui.c
    ${THISTLE_DIR}/components/apps_builtin/settings/settings_app.c
    ${THISTLE_DIR}/components/apps_builtin/settings/settings_ui.c
    ${THISTLE_DIR}/components/apps_builtin/messenger/messenger_app.c
    ${THISTLE_DIR}/components/apps_builtin/messenger/messenger_ui.c
    ${THISTLE_DIR}/components/apps_builtin/messenger/messenger_transport.c
    ${THISTLE_DIR}/components/apps_builtin/assistant/assistant_app.c
    ${THISTLE_DIR}/components/apps_builtin/assistant/assistant_config.c
    ${THISTLE_DIR}/components/apps_builtin/assistant/assistant_json.c
    ${THISTLE_DIR}/components/apps_builtin/assistant/assistant_request.c
    ${THISTLE_DIR}/components/apps_builtin/assistant/assistant_setup.c
    ${THISTLE_DIR}/components/apps_builtin/assistant/assistant_ui.c
    ${THISTLE_DIR}/components/apps_builtin/terminal/terminal_app.c
    ${THISTLE_DIR}/components/apps_builtin/terminal/terminal_ui.c
)

include(${THISTLE_DIR}/cmake/thistle_apps.cmake)
set(THISTLE_APP_INCLUDE_DIRS)
thistle_collect_optional_apps()

set(THISTLE_SIM_SRCS)
foreach(source IN LISTS THISTLE_SRCS)
    if(EXISTS "${source}")
        list(APPEND THISTLE_SIM_SRCS "${source}")
    else()
        message(FATAL_ERROR "Missing source: ${source}")
    endif()
endforeach()

# Simulator sources
set(SIM_SRCS
    main.c
    platform/platform_stubs.c
    platform/sim_vfs.c
    platform/sim_http.c
    platform/sim_i2c_bus.c
    platform/sim_spi_bus.c
    devices/dev_pcf8563.c
    devices/dev_qmi8658c.c
    devices/dev_tca8418.c
    devices/dev_cst328.c
    devices/dev_cst66xx.c
    devices/dev_ltr553.c
    devices/dev_xl9555.c
    drivers/sim_display.c
    drivers/sim_input.c
    drivers/sim_storage.c
    drivers/sim_network.c
    drivers/sim_cellular.c
    drivers/sim_power.c
    drivers/sim_gps.c
    drivers/sim_imu.c
    drivers/sim_haptic.c
    drivers/sim_rtc.c
    drivers/sim_audio.c
    drivers/sim_radio.c
    drivers/sim_scenario.c
    # sim_wifi.c and sim_ble.c removed — Rust kernel provides wifi_manager + ble_manager
    board_simulator.c
    sim_assert.c
    sim_app_loader.c
    sim_host_app_api.c
)

add_executable(thistle_sim ${SIM_SRCS} ${THISTLE_SIM_SRCS} ${LVGL_SRCS})

# Include paths — simulator platform stubs MUST come before any ThistleOS includes
# so that #include "esp_log.h" finds our stub, not a real ESP-IDF header
target_include_directories(thistle_sim PRIVATE
    # Simulator stubs FIRST (shadow ESP-IDF headers)
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers
    ${CMAKE_CURRENT_SOURCE_DIR}/devices
    ${CMAKE_CURRENT_SOURCE_DIR}
    # LVGL
    ${LVGL_DIR}
    ${LVGL_DIR}/src
    # ThistleOS component includes
    ${THISTLE_DIR}/components/thistle_hal/include
    ${THISTLE_DIR}/components/kernel/include
    ${THISTLE_DIR}/components/ui/include
    ${THISTLE_DIR}/components/apps_builtin
    ${THISTLE_APP_INCLUDE_DIRS}
    ${THISTLE_DIR}/app_sdk/include
    # SDL2
    ${SDL2_INCLUDE_DIRS}
)

# LVGL configuration for simulator
target_compile_definitions(thistle_sim PRIVATE
    LV_CONF_INCLUDE_SIMPLE=1
    LV_USE_SDL=1
    LV_USE_OS=LV_OS_NONE
    LV_FONT_MONTSERRAT_14=1
    LV_FONT_MONTSERRAT_18=1
    LV_FONT_MONTSERRAT_22=1
    LV_COLOR_DEPTH=16
    # Disable ESP-IDF specific features
    SIMULATOR_BUILD=1
    THISTLE_HAVE_FILEMGR=${THISTLE_HAVE_FILEMGR}
    THISTLE_HAVE_READER=${THISTLE_HAVE_READER}
    THISTLE_HAVE_PODCASTS=${THISTLE_HAVE_PODCASTS}
    THISTLE_HAVE_NAVIGATOR=${THISTLE_HAVE_NAVIGATOR}
    THISTLE_HAVE_NOTES=${THISTLE_HAVE_NOTES}
    THISTLE_HAVE_APPSTORE=${THISTLE_HAVE_APPSTORE}
    THISTLE_HAVE_WIFISCANNER=${THISTLE_HAVE_WIFISCANNER}
    THISTLE_HAVE_FLASHLIGHT=${THISTLE_HAVE_FLASHLIGHT}
    THISTLE_HAVE_WEATHER=${THISTLE_HAVE_WEATHER}
    THISTLE_HAVE_VAULT=${THISTLE_HAVE_VAULT}
    CONFIG_LOG_DEFAULT_LEVEL=3
    # SD card path for simulator — points to local sdcard directory
    THISTLE_SDCARD_PATH="/tmp/thistle_sdcard"
)

# The Podcasts app's PodcastIndex key and secret, from .env when there is
# one, into a header in this build directory: never the source tree.
include(${THISTLE_DIR}/apps/podcasts/credentials.cmake)
thistle_podcasts_credentials("${CMAKE_CURRENT_BINARY_DIR}/podcasts")
target_include_directories(thistle_sim PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/podcasts")

# Build Rust kernel for host target
set(KERNEL_RS_DIR "${THISTLE_DIR}/components/kernel_rs")
execute_process(
    COMMAND uname -m
    OUTPUT_VARIABLE HOST_ARCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
# Rust uses "aarch64" not "arm64"
if(HOST_ARCH STREQUAL "arm64")
    set(HOST_ARCH "aarch64")
endif()
if(APPLE)
    set(RUST_HOST_TARGET "${HOST_ARCH}-apple-darwin")
else()
    set(RUST_HOST_TARGET "${HOST_ARCH}-unknown-linux-gnu")
endif()
set(RUST_HOST_LIB "${KERNEL_RS_DIR}/target/${RUST_HOST_TARGET}/release/libthistle_kernel.a")
file(GLOB_RECURSE KERNEL_RS_SOURCES CONFIGURE_DEPENDS
    "${KERNEL_RS_DIR}/src/*.rs"
)

add_custom_command(
    OUTPUT ${RUST_HOST_LIB}
    COMMAND ${CMAKE_COMMAND} -E env "PATH=$ENV{HOME}/.cargo/bin:$ENV{PATH}"
        cargo build --release --target ${RUST_HOST_TARGET} --features sim-bus
    WORKING_DIRECTORY ${KERNEL_RS_DIR}
    DEPENDS
        ${KERNEL_RS_SOURCES}
        "${KERNEL_RS_DIR}/Cargo.toml"
        "${KERNEL_RS_DIR}/Cargo.lock"
    COMMENT "Building Rust kernel for simulator (${RUST_HOST_TARGET})"
    VERBATIM
)
add_custom_target(kernel_rs_host_build DEPENDS ${RUST_HOST_LIB})
add_dependencies(thistle_sim kernel_rs_host_build)

target_include_directories(thistle_sim PRIVATE
    ${KERNEL_RS_DIR}/include
)

target_link_directories(thistle_sim PRIVATE ${SDL2_LIBRARY_DIRS})
target_link_libraries(thistle_sim PRIVATE ${SDL2_LIBRARIES} ${RUST_HOST_LIB} pthread m curl dl)

# Export symbols so dlopen'd modules can resolve syscall functions
if(NOT APPLE)
    target_link_options(thistle_sim PRIVATE -rdynamic)
else()
    target_link_options(thistle_sim PRIVATE -Wl,-export_dynamic)
endif()

# Suppress warnings from LVGL and external code
target_compile_options(thistle_sim PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-unused-function
    -Wno-unused-variable
)

# --- Simulator unit tests ---
add_executable(thistle_sim_tests
    tests/test_sim_main.c
    tests/test_sim_assert.c
    tests/test_i2c_bus.c
    tests/test_cst66xx.c
    tests/test_wm_select.c
    tests/stub_wm_vtables.c
    ${THISTLE_DIR}/components/ui/src/wm_select.c
    tests/test_epaper_refresh_policy.c
    ${THISTLE_DIR}/components/ui/src/epaper_refresh.c
    tests/test_pull_gesture.c
    ${THISTLE_DIR}/components/ui/src/pull_gesture.c
    tests/test_xl9555.c
    tests/test_board_fingerprint.c
    tests/test_http_client_shim.c
    sim_assert.c
    platform/sim_i2c_bus.c
    platform/sim_spi_bus.c
    platform/sim_http.c
    ${THISTLE_DIR}/components/kernel_rs/http_client_shim.c
    ${THISTLE_DIR}/components/kernel/src/board_fingerprint.c
    platform/platform_stubs.c
    devices/dev_pcf8563.c
    devices/dev_qmi8658c.c
    devices/dev_tca8418.c
    devices/dev_cst328.c
    devices/dev_cst66xx.c
    devices/dev_ltr553.c
    devices/dev_xl9555.c
)

target_include_directories(thistle_sim_tests PRIVATE
    ${THISTLE_DIR}/components/ui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers
    ${CMAKE_CURRENT_SOURCE_DIR}/devices
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${THISTLE_DIR}/components/thistle_hal/include
    ${THISTLE_DIR}/components/kernel/include
    ${THISTLE_DIR}/components/kernel_rs/include
)

target_compile_definitions(thistle_sim_tests PRIVATE
    SIMULATOR_BUILD=1
)

target_compile_options(thistle_sim_tests PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-unused-function
    -Wno-unused-variable
)

target_link_libraries(thistle_sim_tests PRIVATE pthread m curl)
