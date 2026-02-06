#include "wifi_manager.h"

#include <string.h>

#include "config.h"
#include "config_manager.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "nvs.h"
#include "nvs_flash.h"
#ifdef CONFIG_HAS_SDCARD
#include "sdcard.h"
#endif
static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool s_is_connected = false;

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                          void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 5) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        s_is_connected = false;
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_is_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create both STA and AP network interfaces
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    // Set DHCP hostname
    if (sta_netif) {
        esp_netif_set_hostname(sta_netif, "photoframe");
        ESP_LOGI(TAG, "DHCP hostname set to: photoframe");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, &instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // Don't start WiFi here - let wifi_manager_connect() or wifi_provisioning_start_ap() start it
    // ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_manager_init finished.");

    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID is empty");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *) wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password) {
        strncpy((char *) wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    // Stop WiFi if it's running, then set config
    esp_wifi_stop();

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));  // Enable power save at boot/connect

    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", ssid);
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", ssid);
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        return ESP_FAIL;
    }
}

esp_err_t wifi_manager_disconnect(void)
{
    s_is_connected = false;
    return esp_wifi_disconnect();
}

bool wifi_manager_is_connected(void)
{
    return s_is_connected;
}

esp_err_t wifi_manager_get_ip(char *ip_str, size_t len)
{
    if (!ip_str || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return ESP_FAIL;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);
    if (ret != ESP_OK) {
        return ret;
    }

    snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_WIFI_SSID_KEY, ssid);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_WIFI_PASS_KEY, password);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    return err;
}

esp_err_t wifi_manager_load_credentials(char *ssid, char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_len = WIFI_SSID_MAX_LEN;
    err = nvs_get_str(nvs_handle, NVS_WIFI_SSID_KEY, ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    size_t pass_len = WIFI_PASS_MAX_LEN;
    err = nvs_get_str(nvs_handle, NVS_WIFI_PASS_KEY, password, &pass_len);
    nvs_close(nvs_handle);

    return err;
}

EventGroupHandle_t wifi_manager_get_event_group(void)
{
    return s_wifi_event_group;
}

#ifdef CONFIG_HAS_SDCARD
esp_err_t wifi_manager_load_credentials_from_sdcard(char *ssid, char *password)
{
    if (!sdcard_is_mounted()) {
        return ESP_ERR_NOT_FOUND;
    }

    const char *wifi_file = "/sdcard/wifi.txt";
    FILE *fp = fopen(wifi_file, "r");

    if (!fp) {
        ESP_LOGD(TAG, "No wifi.txt found on SD card");
        return ESP_ERR_NOT_FOUND;
    }

    // Read SSID (first line)
    if (!fgets(ssid, WIFI_SSID_MAX_LEN, fp)) {
        ESP_LOGE(TAG, "Failed to read SSID from wifi.txt");
        fclose(fp);
        return ESP_FAIL;
    }

    // Remove trailing newline/carriage return
    size_t len = strlen(ssid);
    while (len > 0 && (ssid[len - 1] == '\n' || ssid[len - 1] == '\r')) {
        ssid[len - 1] = '\0';
        len--;
    }

    if (len == 0) {
        ESP_LOGE(TAG, "SSID is empty in wifi.txt");
        fclose(fp);
        return ESP_FAIL;
    }

    // Read password (second line)
    if (!fgets(password, WIFI_PASS_MAX_LEN, fp)) {
        ESP_LOGE(TAG, "Failed to read password from wifi.txt");
        fclose(fp);
        return ESP_FAIL;
    }

    // Remove trailing newline/carriage return
    len = strlen(password);
    while (len > 0 && (password[len - 1] == '\n' || password[len - 1] == '\r')) {
        password[len - 1] = '\0';
        len--;
    }

    // Read device name (third line, optional)
    char device_name[DEVICE_NAME_MAX_LEN] = {0};
    if (fgets(device_name, DEVICE_NAME_MAX_LEN, fp)) {
        // Remove trailing newline/carriage return
        len = strlen(device_name);
        while (len > 0 && (device_name[len - 1] == '\n' || device_name[len - 1] == '\r')) {
            device_name[len - 1] = '\0';
            len--;
        }

        // Only set if not empty
        if (len > 0) {
            config_manager_set_device_name(device_name);
            ESP_LOGI(TAG, "Device name loaded from SD card: %s", device_name);
        }
    }

    fclose(fp);

    ESP_LOGI(TAG, "WiFi credentials loaded from SD card: SSID=%s", ssid);

    // Delete wifi.txt to prevent re-reading on next boot
    // This prevents infinite loop if credentials are invalid
    if (unlink(wifi_file) == 0) {
        ESP_LOGI(TAG, "Deleted wifi.txt from SD card");
    } else {
        ESP_LOGW(TAG, "Failed to delete wifi.txt - may cause issues if credentials are invalid");
    }

    return ESP_OK;
}
#endif