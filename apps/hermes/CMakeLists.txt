include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS
        "hermes/hermes_app.c"
        "hermes/hermes_ui.c"
        ${THISTLE_HERMES_SRCS}
    INCLUDE_DIRS "."
    # podcasts: its JSON reader and SHA-1, which the WebSocket handshake uses.
    REQUIRES kernel ui thistle_hal podcasts esp_timer lwip
)

set_source_files_properties(${THISTLE_HERMES_THIRD_PARTY_SRCS}
    TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES COMPILE_OPTIONS "-w")

# The bridge's address from .env, into a header in the build directory: see
# bridge_url.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/bridge_url.cmake)
thistle_hermes_bridge_url("${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
