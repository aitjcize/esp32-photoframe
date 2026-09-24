#include "rotation_network.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "ha_integration.h"
#include "ota_manager.h"
#include "utils.h"
#include "wifi_manager.h"

static const char *TAG = "rotation_network";

void rotation_network_done(void)
{
    // Local-only wakes may never initialize esp_netif/lwIP. Even SNTP stop
    // must not issue a TCP/IP operation before that initialization.
    if (!wifi_manager_get_event_group()) {
        return;
    }
    // The response may have enabled HA or requested a config window.
    if (ha_is_configured() || utils_get_post_rotate_wait_sec() > 0) {
        return;
    }
    if (!ota_wait_for_check(30000)) {
        ESP_LOGI(TAG, "OTA check still active; keeping WiFi on for this rotation");
        return;
    }
    esp_sntp_stop();
    esp_err_t err = wifi_manager_stop();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Network work complete; WiFi off before image processing and refresh");
    } else {
        ESP_LOGW(TAG, "Could not stop WiFi before refresh: %s", esp_err_to_name(err));
    }
}
