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
        "src/lvgl_wm.c"
        "src/lvgl_wm_epaper.c"
        "src/lvgl_wm_lcd.c"
        "src/wm_select.c"
        "src/widget_shims.c"
    INCLUDE_DIRS "include"
    REQUIRES thistle_hal kernel esp_timer lvgl__lvgl
)
