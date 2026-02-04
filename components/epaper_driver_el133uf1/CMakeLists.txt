set(srcs "")
if(CONFIG_EP_DRIVER_EL133UF1)
    list(APPEND srcs "src/driver_el133uf1.c")
endif()

idf_component_register(
    SRCS "${srcs}"
    REQUIRES driver esp_driver_gpio esp_timer driver esp_driver_spi epaper
)
