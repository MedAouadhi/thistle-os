include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS
        "podcasts/podcasts_app.c"
        "podcasts/podcasts_ui.c"
        ${THISTLE_PODCASTS_SRCS}
    INCLUDE_DIRS "."
    REQUIRES kernel ui thistle_hal reader esp_http_client esp_timer mbedtls pthread
)

set_source_files_properties(${THISTLE_PODCASTS_THIRD_PARTY_SRCS}
    TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES COMPILE_OPTIONS "-w")

# The PodcastIndex key and secret from .env, into a header in the build
# directory: see credentials.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/credentials.cmake)
thistle_podcasts_credentials("${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
