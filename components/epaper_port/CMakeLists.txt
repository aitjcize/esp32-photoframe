set(srcs "")
if(CONFIG_EP_DRIVER_SPECTRA_E6)
    list(APPEND srcs "src/driver_spectra_e6.c")
elseif(CONFIG_EP_DRIVER_SEEEDSTUDIO_T133A01)
    list(APPEND srcs "src/driver_seeedstudio_t133a01.c")
endif()

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "./include"
    REQUIRES driver esp_timer epaper_src esp_driver_gpio esp_driver_spi
)
