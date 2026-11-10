# The Transit app's sources beyond transit_app.c and transit_ui.c, relative
# to this directory. Named once, here, for the firmware component
# (CMakeLists.txt) and both simulators (cmake/thistle_apps.cmake). Each
# city's Provider is a file in transit/providers/, found here by name, so
# adding a city adds no line to this file (transit_provider.h).
file(GLOB _thistle_transit_providers RELATIVE "${CMAKE_CURRENT_LIST_DIR}"
     "${CMAKE_CURRENT_LIST_DIR}/transit/providers/*.c")
list(SORT _thistle_transit_providers)
set(THISTLE_TRANSIT_SRCS
    transit/transit_net.c
    transit/transit_providers.c
    transit/transit_session.c
    ${_thistle_transit_providers}
)
unset(_thistle_transit_providers)
