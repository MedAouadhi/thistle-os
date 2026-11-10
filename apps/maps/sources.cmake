# The Maps app's sources beyond maps_app.c and maps_ui.c, relative to this
# directory. Named once, here, for the firmware component (CMakeLists.txt)
# and both simulators (cmake/thistle_apps.cmake). The map itself is
# components/map_engine's.
set(THISTLE_MAPS_SRCS
    maps/maps_layers.c
)
