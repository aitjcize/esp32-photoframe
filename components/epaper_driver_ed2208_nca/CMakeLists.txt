set(srcs "")
if(CONFIG_EP_DRIVER_ED2208_NCA)
    list(APPEND srcs "src/driver_ed2208_nca.c")
endif()

idf_component_register(
    SRCS "${srcs}"
    REQUIRES driver esp_driver_gpio esp_timer driver esp_driver_spi epaper
)
