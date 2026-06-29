set(srcs "")
if(CONFIG_EP_DRIVER_IT8951)
    list(APPEND srcs "src/driver_it8951.c")
endif()

idf_component_register(
    SRCS "${srcs}"
    REQUIRES driver esp_driver_gpio esp_timer esp_driver_spi epaper
)
