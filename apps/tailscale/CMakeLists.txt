idf_component_register(
    SRCS
        "tailscale/tailscale_app.c"
        "tailscale/tailscale_ui.c"
    INCLUDE_DIRS "."
    REQUIRES kernel ui thistle_hal
)
