# Shared battery-voltage ADC helper used by the ADC-based boards.
set(SRCS "src/battery_adc.c")

set(PRIV_REQS
    charger_driver_sy6974b
    epaper_driver_ed2208_gca
    epaper_driver_ed2208_nca
    epaper_driver_it8951
    esp_driver_i2c
    esp_driver_spi
    pmic_driver_axp2101
    rtc_driver_pcf85063
    rtc_driver_pcf8563
    sdcard
    sensor_driver_sht3x
    sensor_driver_sht40
    sensor_driver_shtc3
)

if(CONFIG_BOARD_DRIVER_WAVESHARE_PHOTOPAINTER_73)
    list(APPEND SRCS "src/driver_waveshare_photopainter_73.c")
elseif(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_XIAO_EE02)
    list(APPEND SRCS "src/driver_seeedstudio_xiao_ee02.c")
elseif(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_XIAO_EE03)
    list(APPEND SRCS "src/driver_seeedstudio_xiao_ee03.c")
elseif(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_XIAO_EE04)
    list(APPEND SRCS "src/driver_seeedstudio_xiao_ee04.c")
elseif(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_RETERMINAL_E1002)
    list(APPEND SRCS "src/driver_seeedstudio_reterminal_e1002.c")
elseif(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_RETERMINAL_E1003)
    list(APPEND SRCS "src/driver_seeedstudio_reterminal_e1003.c")
elseif(CONFIG_BOARD_DRIVER_SEEEDSTUDIO_RETERMINAL_E1004)
    list(APPEND SRCS "src/driver_seeedstudio_reterminal_e1004.c")
elseif(CONFIG_BOARD_DRIVER_M5STACK_M5PAPER_V11)
    list(APPEND SRCS "src/driver_m5stack_m5paper_v11.c")
endif()

set(REQS esp_timer driver esp_adc esp_driver_gpio epaper sensor)

# Only the boards on chips that have the peripheral use it to detect USB power;
# the component isn't meaningful on a plain ESP32 (e.g. the M5Paper).
if(CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED)
    list(APPEND REQS esp_driver_usb_serial_jtag)
endif()

idf_component_register(
    SRCS "${SRCS}"
    INCLUDE_DIRS "include"
    REQUIRES ${REQS}
    PRIV_REQUIRES ${PRIV_REQS}
)
