set(SOURCES 
    "main.c"
    "wifi_manager.c"
    "wifi_provisioning.c"
    "dns_server.c"
    "http_server.c"
    "image_processor.c"
    "display_manager.c"
    "power_manager.c"
    "mdns_service.c"
)

set(INCLUDE_DIRS ".")

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_sdmmc nvs_flash esp_wifi esp_netif esp_event esp_http_server fatfs json espressif__esp_jpeg ListLib axpPower i2c_bsp espressif__mdns
    PRIV_REQUIRES epaper_port epaper_src sdcard_bsp
    EMBED_FILES "webapp/index.html" "webapp/style.css" "webapp/app.js" "webapp/image-processor.js" "webapp/favicon.svg" "webapp/provision.html"
)
