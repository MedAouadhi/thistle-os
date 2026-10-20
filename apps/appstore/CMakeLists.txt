include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS
        "appstore/appstore_app.c"
        "appstore/appstore_ui.c"
        ${THISTLE_APPSTORE_SRCS}
    INCLUDE_DIRS "."
    REQUIRES kernel ui thistle_hal mbedtls esp_http_client
)
