include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS
        "transit/transit_app.c"
        "transit/transit_ui.c"
        ${THISTLE_TRANSIT_SRCS}
    INCLUDE_DIRS "."
    # map_engine: the map. reader: its Latin-1 font, for the credit's ©.
    # podcasts: its JSON reader, for the Provider's answers.
    REQUIRES kernel ui thistle_hal map_engine reader podcasts esp_http_client esp_timer mbedtls
)
