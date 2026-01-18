#include "ha_integration.h"

#include <string.h>

#include "axp_prot.h"
#include "cJSON.h"
#include "config_manager.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_manager.h"
#include "utils.h"

static const char *TAG = "ha_integration";

static TaskHandle_t battery_push_task_handle = NULL;
static int64_t next_battery_push_time = 0;  // Use absolute time for battery push

bool ha_is_configured(void)
{
    const char *ha_url = config_manager_get_ha_url();
    return (ha_url != NULL && strlen(ha_url) > 0);
}

esp_err_t ha_post_battery_info(void)
{
    const char *ha_url = config_manager_get_ha_url();

    // Check if HA URL is configured
    if (!ha_is_configured()) {
        ESP_LOGD(TAG, "HA URL not configured, skipping battery post");
        return ESP_OK;  // Not an error, just not configured
    }

    // Build the API endpoint URL
    char url[512];
    snprintf(url, sizeof(url), "%s/api/esp32_photoframe/battery", ha_url);

    // Build JSON payload with all battery fields using shared function
    cJSON *json = create_battery_json();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to create battery JSON");
        return ESP_FAIL;
    }

    char *json_payload = cJSON_PrintUnformatted(json);
    if (json_payload == NULL) {
        ESP_LOGE(TAG, "Failed to print JSON payload");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Posting battery status to HA: %s", json_payload);

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .user_agent = "ESP32-PhotoFrame/1.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    // Set headers and data
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_payload, strlen(json_payload));

    // Perform request
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);
    free(json_payload);
    cJSON_Delete(json);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        return err;
    }

    if (status_code != 200) {
        ESP_LOGW(TAG, "HA returned HTTP %d", status_code);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Battery status posted to HA successfully");
    return ESP_OK;
}

static void battery_push_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Push battery data to HA whenever device is awake and HA is configured
        // Device is awake when HTTP server is running (not in deep sleep)
        if (!ha_is_configured()) {
            next_battery_push_time = 0;  // Reset when HA not configured
            continue;
        }

        // Handle periodic battery push when device is awake
        int64_t now = esp_timer_get_time();  // Get absolute time in microseconds

        if (next_battery_push_time == 0) {
            // Initialize next battery push time (60 seconds)
            next_battery_push_time = now + (60 * 1000000LL);
            ESP_LOGI(TAG, "Battery push scheduled in 60 seconds");
        } else if (now >= next_battery_push_time) {
            // Time to push battery data and OTA version info
            ESP_LOGI(TAG, "Pushing battery data and OTA version info to HA");

            esp_err_t err = ha_post_battery_info();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to push battery data to HA");
            }

            err = ha_post_ota_version_info();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to push OTA version info to HA");
            }

            // Schedule next push (60 seconds)
            next_battery_push_time = now + (60 * 1000000LL);
        }
    }
}

esp_err_t ha_integration_init(void)
{
    if (battery_push_task_handle != NULL) {
        ESP_LOGW(TAG, "HA integration already initialized");
        return ESP_OK;
    }

    xTaskCreate(battery_push_task, "battery_push", 8192, NULL, 5, &battery_push_task_handle);
    ESP_LOGI(TAG, "HA integration initialized");

    // Do an initial push of battery and OTA data if HA is configured
    if (ha_is_configured()) {
        ESP_LOGI(TAG, "Performing initial push to HA");

        esp_err_t err = ha_post_battery_info();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to push initial battery data to HA");
        }
    }

    return ESP_OK;
}

esp_err_t ha_post_ota_version_info(void)
{
    const char *ha_url = config_manager_get_ha_url();

    // Check if HA URL is configured
    if (!ha_is_configured()) {
        ESP_LOGD(TAG, "HA URL not configured, skipping OTA version post");
        return ESP_OK;
    }

    // Get OTA status
    ota_status_t status;
    ota_get_status(&status);

    // Build the API endpoint URL
    char url[512];
    snprintf(url, sizeof(url), "%s/api/esp32_photoframe/ota", ha_url);

    // Build JSON payload with version info and button entity
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to create OTA JSON");
        return ESP_FAIL;
    }

    cJSON_AddStringToObject(json, "current_version", status.current_version);
    cJSON_AddStringToObject(json, "latest_version", status.latest_version);
    cJSON_AddStringToObject(json, "state",
                            status.state == OTA_STATE_IDLE               ? "idle"
                            : status.state == OTA_STATE_CHECKING         ? "checking"
                            : status.state == OTA_STATE_UPDATE_AVAILABLE ? "update_available"
                            : status.state == OTA_STATE_DOWNLOADING      ? "downloading"
                            : status.state == OTA_STATE_INSTALLING       ? "installing"
                            : status.state == OTA_STATE_SUCCESS          ? "success"
                                                                         : "error");

    char *json_payload = cJSON_PrintUnformatted(json);
    if (json_payload == NULL) {
        ESP_LOGE(TAG, "Failed to print OTA JSON payload");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Posting OTA version info to HA: %s", json_payload);

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .user_agent = "ESP32-PhotoFrame/1.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(json_payload);
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    // Set headers and data
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_payload, strlen(json_payload));

    // Perform request
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);
    free(json_payload);
    cJSON_Delete(json);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        return err;
    }

    if (status_code != 200) {
        ESP_LOGW(TAG, "HA returned HTTP %d", status_code);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA version info posted to HA successfully");
    return ESP_OK;
}
