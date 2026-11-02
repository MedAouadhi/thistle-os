include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS
        "tailchat/tailchat_app.c"
        "tailchat/tailchat_ui.c"
        ${THISTLE_TAILCHAT_SRCS}
    INCLUDE_DIRS "."
    REQUIRES kernel ui thistle_hal opus
)
