set(SOURCES 
    "ai_manager.c"
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
    "testable_utils.c"
    "utils.c"
    "wifi_manager.c"
    "wifi_provisioning.c"
)

set(INCLUDE_DIRS ".")

set(PRIV_REQUIRES
    app_update
    board_hal
    epaper
    epaper_src
    esp_app_format
    esp_driver_gpio
    esp_event
    esp_http_client
    esp_http_server
    esp_https_ota
    esp_netif
    esp_timer
    esp_wifi
    espressif__cjson
    espressif__esp_jpeg
    espressif__libpng
    espressif__mdns
    fatfs
    mbedtls
    memfs
    nvs_flash
    sdcard
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
    REQUIRES ""
    PRIV_REQUIRES ${PRIV_REQUIRES} 
    EMBED_FILES "webapp/index.html" "webapp/favicon.svg" "webapp/assets/index.css" "webapp/assets/index.js" "webapp/assets/index2.js" "webapp/assets/exif-reader.js" "webapp/assets/browser.js" "webapp/assets/__vite-browser-external.js" "resources/measurement_sample.jpg"
    EMBED_TXTFILES "certs/openai_root.pem"
)
