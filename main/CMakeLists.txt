set(SOURCES 
    "album_manager.c"
    "color_palette.c"
    "config_manager.c"
    "display_manager.c"
    "dns_server.c"
    "ha_integration.c"
    "http_server.c"
    "image_processor.c"
    "main.c"
    "mdns_service.c"
    "ota_manager.c"
    "periodic_tasks.c"
    "png_decoder.c"
    "power_manager.c"
    "processing_settings.c"
    "utils.c"
    "wifi_manager.c"
    "wifi_provisioning.c"
)

set(INCLUDE_DIRS ".")

set(MAIN_REQUIRES esp_driver_gpio esp_driver_sdmmc nvs_flash esp_wifi esp_netif esp_event esp_http_server fatfs espressif__cjson espressif__esp_jpeg ListLib espressif__mdns espressif__libpng)

# WaveShare-specific dependencies (AXP power management, I2C bus)
if(CONFIG_BOARD_DRIVER_WAVESHARE_PHOTOPAINTER_73)
    list(APPEND MAIN_REQUIRES axpPower i2c_bsp)
endif()

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
    REQUIRES ${MAIN_REQUIRES}
    PRIV_REQUIRES epaper_port epaper_src sdcard_bsp esp_timer esp_app_format esp_http_client esp_https_ota app_update mbedtls board_hal memfs
    EMBED_FILES "webapp/index.html" "webapp/favicon.svg" "webapp/assets/index.css" "webapp/assets/index.js" "webapp/assets/index2.js" "webapp/assets/exif-reader.js" "webapp/assets/browser.js" "webapp/assets/__vite-browser-external.js" "resources/calibration.png"
)
