idf_component_register(
    SRCS
        "src/manager.c"
        "src/theme.c"
        "src/statusbar.c"
        "src/app_switcher.c"
        "src/epaper_refresh.c"
        "src/toast.c"
        "src/power_ui.c"
        "src/pull_gesture.c"
        "src/quick_panel.c"
        "src/quick_panel_tiles.c"
        "src/call_ui.c"
        "src/dictation.c"
        "src/dictation_core.c"
        "src/arabic_shape.c"
        "src/arabic_text.c"
        "src/fonts/arabic_font_14.c"
        "src/fonts/arabic_font_14_mono.c"
        "src/fonts/arabic_font_18.c"
        "src/fonts/arabic_font_18_mono.c"
        "src/fonts/arabic_font_22.c"
        "src/fonts/arabic_font_22_mono.c"
        "src/lvgl_wm.c"
        "src/lvgl_wm_epaper.c"
        "src/lvgl_wm_lcd.c"
        "src/wm_select.c"
        "src/widget_shims.c"
    INCLUDE_DIRS "include"
    REQUIRES thistle_hal kernel esp_timer lvgl__lvgl esp_http_client mbedtls
)

# Dictation's Cohere key from .env, into a header in the build directory:
# see dictation_credentials.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/dictation_credentials.cmake)
thistle_dictation_credentials("${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Arabic in every label and text field: arabic_text.c stands in for these
# LVGL functions wherever LVGL calls them from another file. The simulator's
# CMakeLists.txt wraps the same list.
foreach(fn IN ITEMS lv_font_get_glyph_dsc lv_font_get_glyph_width lv_draw_label
                    lv_text_get_size lv_label_get_letter_pos lv_label_get_letter_on
                    lv_label_is_char_under_pos)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${fn}")
endforeach()
