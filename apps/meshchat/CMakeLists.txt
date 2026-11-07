include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS
        "meshchat/meshchat_app.c"
        "meshchat/meshchat_ui.c"
        ${THISTLE_MESHCHAT_SRCS}
    INCLUDE_DIRS "."
    REQUIRES kernel ui thistle_hal chatkit tailchat
)
