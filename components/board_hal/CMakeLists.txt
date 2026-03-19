set(SRCS "")

set(PRIV_REQS
    epaper_driver_ed2208_gca
    epaper_driver_ed2208_nca
    esp_driver_i2c
    esp_driver_spi
    pmic_driver_axp2101
    pmic_driver_sy6974b
    rtc_driver_pcf85063
    rtc_driver_pcf8563
    sdcard
    sensor_driver_sht40
    sensor_driver_shtc3
)

if(CONFIG_BOARD_DRIVER_WAVESHARE_PHOTOPAINTER_73)
    list(APPEND SRCS "src/driver_waveshare_photopainter_73.c")
elseif(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_XIAO_EE02)
    list(APPEND SRCS "src/driver_seeedstudio_xiao_ee02.c")
elseif(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_XIAO_EE04)
    list(APPEND SRCS "src/driver_seeedstudio_xiao_ee04.c")
elseif(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_RETERMINAL_E1002)
    list(APPEND SRCS "src/driver_seeedstudio_reterminal_e1002.c")
elseif(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_RETERMINAL_E1004)
    list(APPEND SRCS "src/driver_seeedstudio_reterminal_e1004.c")
endif()

idf_component_register(
    SRCS "${SRCS}"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer driver esp_adc esp_driver_gpio esp_driver_usb_serial_jtag epaper sensor
    PRIV_REQUIRES ${PRIV_REQS}
)
