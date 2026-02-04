idf_component_register(SRCS "pcf85063_rtc.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_driver_i2c)
