# The SMS and MeshCore apps' screens, in Chat's look: no app of its own.
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS ${THISTLE_CHATKIT_SRCS}
    INCLUDE_DIRS "."
    REQUIRES kernel ui thistle_hal tailchat reader
)
