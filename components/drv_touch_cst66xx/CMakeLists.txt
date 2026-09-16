# Header only: the driver itself is Rust, in components/kernel_rs, and reaches
# the link through kernel_rs's WHOLE_ARCHIVE. This component exists so the
# board layer has a config struct to fill in.
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES thistle_hal driver esp_driver_i2c esp_driver_gpio esp_timer
)
