idf_component_register(
    SRCS "src/drv_modem_a7682e.c" "src/a7682e_lifecycle.c" "src/a7682e_net.c"
         "src/a7682e_parse.c" "src/a7682e_sms.c" "src/a7682e_call.c"
    INCLUDE_DIRS "include"
    REQUIRES thistle_hal driver esp_driver_gpio esp_driver_uart esp_netif esp_event
    PRIV_REQUIRES kernel_rs esp_timer
)
