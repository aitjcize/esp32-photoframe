set(srcs "")

if(CONFIG_BOARD_DRIVER_WAVESHARE_PHOTOPAINTER_73)
    list(APPEND srcs "src/driver_waveshare_photopainter_73.c")
elseif(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_XIAO_EE02)
    list(APPEND srcs "src/driver_seeedstudio_xiao_ee02.c")
elseif(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_RETERMINAL_E1002)
    list(APPEND srcs "src/driver_seeedstudio_reterminal_e1002.c")
endif()

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer driver esp_adc esp_driver_gpio epaper_port
    PRIV_REQUIRES axpPower shtc3_sensor sht40_sensor pcf85063_rtc sdcard
)
