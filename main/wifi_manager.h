#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_disconnect(void);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_get_ip(char *ip_str, size_t len);
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_load_credentials(char *ssid, char *password);
esp_err_t wifi_manager_load_credentials_from_sdcard(char *ssid, char *password);
EventGroupHandle_t wifi_manager_get_event_group(void);

#endif
