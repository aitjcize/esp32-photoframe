set(SRCS "")
set(PRIV_REQUIRES sdmmc)

if(CONFIG_SDCARD_DRIVER_SDIO)
    list(APPEND SRCS "src/sdcard_sdio.c")
    list(APPEND PRIV_REQUIRES esp_driver_sdmmc)
endif()

if(CONFIG_SDCARD_DRIVER_SPI)
    list(APPEND SRCS "src/sdcard_spi.c")
    list(APPEND PRIV_REQUIRES esp_driver_spi)
endif()

if(CONFIG_SDCARD_DRIVER_INTERNAL_FLASH)
    list(APPEND SRCS "src/sdcard_internal.c")
    list(APPEND PRIV_REQUIRES wear_levelling)
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio driver fatfs
    PRIV_REQUIRES ${PRIV_REQUIRES}
)