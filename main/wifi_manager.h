#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_wifi.h"
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
int wifi_manager_scan(wifi_ap_record_t *results, int max_results);

// Bring up a secured soft-AP for the persistent AP-only mode (different from
// the open provisioning AP). Sets a static IP of 192.168.4.1, starts the
// DHCP server, and registers a captive-portal-style DNS responder. The
// caller is responsible for starting the HTTP server afterwards.
esp_err_t wifi_manager_start_ap(const char *ssid, const char *password);

// Generate an 8-character random password using a curated alphabet that
// excludes ambiguous characters (0/O, 1/l/I). Buffer must hold at least 9
// bytes (8 chars + null terminator).
void wifi_manager_generate_ap_password(char *out, size_t out_len);

#endif
