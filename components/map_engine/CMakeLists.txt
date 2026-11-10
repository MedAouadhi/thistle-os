# The map engine: reads Map Packs and shows them (docs/maps/). A library
# with no app of its own; the Maps app embeds it, and other built-in apps
# may too.
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS ${THISTLE_MAP_ENGINE_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl
    # The ROM's inflater (miniz.h); heap_caps for PSRAM; esp_timer to time
    # a view's composition.
    PRIV_REQUIRES esp_rom heap esp_timer
)
