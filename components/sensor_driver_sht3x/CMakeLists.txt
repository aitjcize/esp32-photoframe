set(srcs "")
if(CONFIG_SENSOR_DRIVER_SHT3X)
    list(APPEND srcs "src/sht3x_sensor.c")
endif()

idf_component_register(
    SRCS "${srcs}"
    REQUIRES driver esp_driver_i2c sensor
)
