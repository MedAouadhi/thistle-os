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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(thistle_os)

# LVGL's memory pool is a 64 KB static array, and in internal RAM it was the
# largest single user of it. Where PSRAM can hold zeroed statics, it goes
# there; that is the only array LVGL marks this way.
if(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_definitions(${lvgl_lib} PRIVATE
        "LV_ATTRIBUTE_LARGE_RAM_ARRAY=__attribute__((section(\".ext_ram.bss.lvgl_pool\")))")
endif()
