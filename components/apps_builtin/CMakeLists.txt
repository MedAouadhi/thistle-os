idf_component_register(
    SRCS
        "lvgl_apps.c"
        "launcher/launcher_app.c"
        "launcher/launcher_ui.c"
        "settings/settings_app.c"
        "settings/settings_ui.c"
        "messenger/messenger_app.c"
        "messenger/messenger_transport.c"
        "messenger/messenger_ui.c"
        "assistant/assistant_app.c"
        "assistant/assistant_config.c"
        "assistant/assistant_json.c"
        "assistant/assistant_request.c"
        "assistant/assistant_setup.c"
        "assistant/assistant_ui.c"
        "terminal/terminal_app.c"
        "terminal/terminal_ui.c"
    INCLUDE_DIRS "."
    REQUIRES kernel ui thistle_hal mbedtls esp_http_client
    # lvgl_apps.c is the only caller of the apps that live as their own
    # components under apps/, and lvgl_apps.h exposes none of them, so this
    # dependency is an implementation detail rather than part of the interface.
    PRIV_REQUIRES appstore file_manager filexfer flashlight navigator notes phone
                  podcasts reader vault weather wifiscanner
)
