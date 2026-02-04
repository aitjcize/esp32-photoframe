set(srcs "")
set(priv_reqs "sdcard" "pmic_driver_axp2101" "rtc_driver_pcf85063" "epaper_driver_el073tf1u5" "sensor_driver_shtc3"
    "epaper_driver_el133uf1" "sensor_driver_sht40")

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
    REQUIRES esp_timer driver esp_adc esp_driver_gpio epaper sensor
    PRIV_REQUIRES ${priv_reqs}
)
