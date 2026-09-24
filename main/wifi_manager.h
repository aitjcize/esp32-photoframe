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
esp_err_t wifi_manager_update_hostname(void);
// Toggle between full-RX performance (WIFI_PS_NONE, low latency / fast web UI)
// and modem power save (WIFI_PS_MIN_MODEM). Idempotent; safe to call every
// second. The policy for when to use which lives in power_manager.
esp_err_t wifi_manager_set_performance_mode(bool enable);
// Apply the configured IP mode to the STA netif (static address or DHCP).
// Called automatically by wifi_manager_connect; exposed for the provisioning
// connection test, which drives esp_wifi directly (#43).
esp_err_t wifi_manager_apply_ip_config(void);
// Connect and wait for an IP, bounded by a time limit. ESP_OK once connected;
// ESP_FAIL when every retry failed, or when the time limit ran out while the
// AP was rejecting the credentials; ESP_ERR_TIMEOUT when the time limit ran
// out for any other reason (DHCP not answering, AP absent or slow). On a
// timeout the attempt is still running: follow up with
// wifi_manager_stop_connecting() or wifi_manager_keep_reconnecting().
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
// Give up on the current connection attempt: no more automatic reconnects,
// and WiFi is stopped. For callers that will not use the network this wake.
void wifi_manager_stop_connecting(void);
// Keep reconnecting with no retry limit until connected (or until sleep stops
// WiFi). For callers that stay awake and want the network whenever it appears.
// The one exception is an AP that keeps rejecting the credentials: after as
// many rejections as a normal connect allows, WIFI_FAIL_BIT is set and the
// retries stop.
void wifi_manager_keep_reconnecting(void);
esp_err_t wifi_manager_disconnect(void);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_get_ip(char *ip_str, size_t len);
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_load_credentials(char *ssid, char *password);
esp_err_t wifi_manager_load_credentials_from_sdcard(char *ssid, char *password);
EventGroupHandle_t wifi_manager_get_event_group(void);
int wifi_manager_scan(wifi_ap_record_t *results, int max_results);

#endif
