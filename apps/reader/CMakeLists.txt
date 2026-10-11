include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS
        "reader/reader_app.c"
        "reader/reader_ui.c"
        ${THISTLE_READER_SRCS}
    INCLUDE_DIRS "."
    REQUIRES kernel ui thistle_hal
)

set_source_files_properties(${THISTLE_READER_THIRD_PARTY_SRCS}
    TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES COMPILE_OPTIONS "-w")
