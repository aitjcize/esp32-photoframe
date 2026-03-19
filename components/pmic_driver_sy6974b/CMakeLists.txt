idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_gpio esp_driver_i2c esp_driver_usb_serial_jtag
)
