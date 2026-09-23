idf_component_register(
    SRCS "src/drv_audio_es8311.c"
    INCLUDE_DIRS "include"
    REQUIRES thistle_hal
    PRIV_REQUIRES kernel_rs esp_driver_i2s esp_timer freertos
)
