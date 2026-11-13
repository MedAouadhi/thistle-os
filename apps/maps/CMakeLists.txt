include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS
        "maps/maps_app.c"
        "maps/maps_ui.c"
        ${THISTLE_MAPS_SRCS}
    INCLUDE_DIRS "."
    # reader: its Latin-1 font, for the credit's ©. podcasts: its JSON
    # reader, for the GeoJSON overlays.
    REQUIRES kernel ui thistle_hal map_engine reader podcasts
)
