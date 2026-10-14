# ThistleOS Kernel — logic is in Rust (kernel_rs), C shims provide FFI bridges.

set(KERNEL_SRCS
    "src/kernel_shims.c" "src/tk_wm_shims.c" "src/sdcard_shim.c"
    "src/link_deps.c" "src/board_fallback.c" "src/board_fingerprint.c"
    "src/board_builtin_drivers.c" "src/power_platform.c" "src/time_platform.c"
)
set(KERNEL_REQUIRES
    thistle_hal kernel_rs esp_timer freertos esp_partition fatfs vfs app_update
    esp_wifi esp_netif esp_event mbedtls esp_http_client driver spiffs nvs_flash
    sdmmc esp_adc bt
)
if(CONFIG_BT_ENABLED)
    list(APPEND KERNEL_SRCS "src/ble_security_shim.c")
endif()

set(KERNEL_PRIV_REQUIRES
    drv_crypto_mbedtls esp_driver_i2c esp_driver_uart esp_driver_spi esp_driver_gpio
    drv_epaper_gdeq031t10 drv_kbd_tca8418 drv_touch_cst328 drv_touch_cst66xx drv_sdcard
    drv_gps_mia_m10q drv_radio_sx1262 drv_audio_es8311 drv_imu_bhi260ap
    pthread lwip esp_rom
)
# The A7682E modem rides on esp_modem, which no board on another target
# carries; main links the S3-only components the same way.
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND KERNEL_PRIV_REQUIRES drv_modem_a7682e)
endif()

idf_component_register(
    SRCS ${KERNEL_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${KERNEL_REQUIRES}
    PRIV_REQUIRES ${KERNEL_PRIV_REQUIRES}
)

if(IDF_TARGET STREQUAL "esp32s3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE THISTLE_HAS_MODEM_A7682E=1)
endif()
