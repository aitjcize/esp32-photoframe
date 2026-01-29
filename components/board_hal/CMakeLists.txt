set(srcs "")
if(CONFIG_BOARD_DRIVER_WAVESHARE_PHOTOPAINTER_73)
    list(APPEND srcs "src/driver_waveshare_photopainter_73.c")
elseif(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_XIAO_EE02)
    list(APPEND srcs "src/driver_seeedstudio_xiao_ee02.c")
endif()

# WaveShare-specific dependencies (AXP power management, I2C bus)
if(CONFIG_BOARD_DRIVER_WAVESHARE_PHOTOPAINTER_73)
    list(APPEND MAIN_REQUIRES axpPower i2c_bsp)
endif()

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer driver esp_adc esp_driver_gpio epaper_port
    PRIV_REQUIRES axpPower shtc3_sensor pcf85063_rtc
)
