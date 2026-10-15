include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS
        "filexfer/filexfer_app.c"
        "filexfer/filexfer_ui.c"
        ${THISTLE_FILEXFER_SRCS}
    INCLUDE_DIRS "."
    # mdns is espressif/mdns, from idf_component.yml.
    REQUIRES kernel ui thistle_hal esp_http_server esp_timer lwip mdns spiffs pthread
)

# The web page, into a header in the build directory: see web.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/web.cmake)
thistle_filexfer_web("${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
