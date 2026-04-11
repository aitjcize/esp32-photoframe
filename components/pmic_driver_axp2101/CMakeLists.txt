idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_gpio esp_driver_i2c esp_timer
)
add_compile_definitions(XPOWERS_CHIP_AXP2101 CONFIG_XPOWERS_ESP_IDF_NEW_API)