cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED IDF_TARGET)
    set(IDF_TARGET "esp32s3")
endif()
set(SDKCONFIG_DEFAULTS
    "sdkconfig.defaults;sdkconfig.defaults.${IDF_TARGET}"
)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components"
    "${CMAKE_CURRENT_SOURCE_DIR}/apps"
)
# The apps the App Store installs are not built in (cmake/thistle_apps.cmake).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/thistle_apps.cmake")
set(EXCLUDE_COMPONENTS ${THISTLE_STORE_APPS})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The firmware's version is its tag: tools/firmware_version.sh describes the
# commit by firmware tags only (`v<digit>...`), so an app's `notes-v1.0.1`
# never becomes it, and drops the `v`: "0.6.0", or on a laptop
# "0.5.2-108-gabc1234-dirty". It lands in the image's description and, through
# kernel_rs/CMakeLists.txt, in the kernel (version.rs). ESP-IDF's own
# describe, which this replaces, matched any tag. Watching HEAD reconfigures
# after a commit or a checkout, as ESP-IDF's own does.
execute_process(
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tools/firmware_version.sh" "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE)
get_git_head_revision(_thistle_git_ref _thistle_git_hash "${CMAKE_CURRENT_SOURCE_DIR}")
# esp_app_desc_t holds 31 characters; the kernel must report the same.
string(SUBSTRING "${PROJECT_VER}" 0 31 PROJECT_VER)
message(STATUS "ThistleOS version: ${PROJECT_VER}")

project(thistle_os)

# LVGL's memory pool is a 64 KB static array, and in internal RAM it was the
# largest single user of it. Where PSRAM can hold zeroed statics, it goes
# there; that is the only array LVGL marks this way.
if(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_definitions(${lvgl_lib} PRIVATE
        "LV_ATTRIBUTE_LARGE_RAM_ARRAY=__attribute__((section(\".ext_ram.bss.lvgl_pool\")))")
endif()
