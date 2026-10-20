include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS
        "todo/todo_app.c"
        "todo/todo_ui.c"
        ${THISTLE_TODO_SRCS}
    INCLUDE_DIRS "."
    REQUIRES kernel ui thistle_hal
)
