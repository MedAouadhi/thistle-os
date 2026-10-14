include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS
        "phone/phone_app.c"
        "phone/phone_ui.c"
        ${THISTLE_PHONE_SRCS}
    INCLUDE_DIRS "."
    REQUIRES kernel ui thistle_hal reader
)
