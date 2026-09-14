set(thistle_main_requires kernel thistle_hal)

if(CONFIG_THISTLE_RUN_TESTS)
    list(APPEND thistle_main_requires unity test_thistle)
endif()

# The remaining built-in C applications are a migration bridge for the S3
# images. Other targets boot the small launcher and discover signed ELF apps
# from SPIFFS/SD, keeping generic non-PSRAM firmware within internal DRAM.
if(IDF_TARGET STREQUAL "esp32s3")
    # apps_builtin pulls in the apps/ components itself; main only calls
    # lvgl_apps_register_builtin().
    list(APPEND thistle_main_requires ui apps_builtin)
endif()

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ${thistle_main_requires}
)

if(IDF_TARGET STREQUAL "esp32s3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE THISTLE_LEGACY_C_APPS=1)
endif()
