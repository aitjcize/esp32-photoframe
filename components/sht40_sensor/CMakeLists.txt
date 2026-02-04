idf_component_register(
    SRCS "src/sht40_sensor.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c
)
