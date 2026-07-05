#include "ha_integration.h"

#include <string.h>

#include "cJSON.h"
#include "config_manager.h"
#include "display_manager.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_manager.h"
#include "utils.h"

static const char *TAG = "ha_integration";

// Accumulates the HTTP response body so we can read the rotation decision the
// HA integration returns in the notify response.
typedef struct {
    char *buf;
    int cap;  // buffer size including the trailing NUL
    int len;
} ha_response_buf_t;

static esp_err_t ha_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != NULL) {
        ha_response_buf_t *rb = (ha_response_buf_t *) evt->user_data;
        int space = rb->cap - 1 - rb->len;
        int n = evt->data_len < space ? evt->data_len : space;
        if (n > 0) {
            memcpy(rb->buf + rb->len, evt->data, n);
            rb->len += n;
            rb->buf[rb->len] = '\0';
        }
    }
    return ESP_OK;
}

bool ha_is_configured(void)
{
    const char *ha_url = config_manager_get_ha_url();
    return (ha_url != NULL && strlen(ha_url) > 0);
}

static esp_err_t ha_send_notification(const char *state, const char *log_message, int timeout_ms,
                                      bool *out_should_rotate)
{
    // Default to rotating unless HA explicitly vetoes it.
    if (out_should_rotate != NULL) {
        *out_should_rotate = true;
    }

    const char *ha_url = config_manager_get_ha_url();

    // Check if HA URL is configured
    if (!ha_is_configured()) {
        ESP_LOGD(TAG, "HA URL not configured, skipping %s notification", state);
        return ESP_OK;  // Not an error, just not configured
    }

    // Build the API endpoint URL (no query parameters)
    char url[512];
    snprintf(url, sizeof(url), "%s/api/esp32_photoframe/notify", ha_url);

    ESP_LOGI(TAG, "%s", log_message);

    // Create JSON payload with device name and state
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return ESP_FAIL;
    }

    const char *device_name = config_manager_get_device_name();
    cJSON_AddStringToObject(root, "device_name", device_name ? device_name : "ESP32-PhotoFrame");
    cJSON_AddStringToObject(root, "device_id", get_device_id());
    cJSON_AddStringToObject(root, "state", state);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return ESP_FAIL;
    }

    // Capture the response body only when the caller wants the rotation decision.
    char resp_body[128] = {0};
    ha_response_buf_t rb = {.buf = resp_body, .cap = sizeof(resp_body), .len = 0};

    // Configure HTTP client for JSON POST
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = timeout_ms,
        .user_agent = "ESP32-PhotoFrame/1.0",
        .event_handler = ha_http_event_handler,
        .user_data = (out_should_rotate != NULL) ? &rb : NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(json_str);
        return ESP_FAIL;
    }

    // Set JSON content type and body
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    // Perform request
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);
    free(json_str);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        return err;
    }

    if (status_code != 200) {
        ESP_LOGW(TAG, "HA returned HTTP %d", status_code);
        return ESP_FAIL;
    }

    // Read the rotation decision from the response, if requested. A successful
    // call that omits or malforms "rotate" leaves the default (rotate).
    if (out_should_rotate != NULL && rb.len > 0) {
        cJSON *resp_json = cJSON_Parse(resp_body);
        if (resp_json != NULL) {
            cJSON *rotate = cJSON_GetObjectItem(resp_json, "rotate");
            if (cJSON_IsBool(rotate)) {
                *out_should_rotate = cJSON_IsTrue(rotate);
            }
            cJSON_Delete(resp_json);
        }
    }

    ESP_LOGI(TAG, "%s notification sent to HA successfully",
             state && strlen(state) > 0 ? state : "online");
    return ESP_OK;
}

esp_err_t ha_notify_online(bool *out_should_rotate)
{
    return ha_send_notification("online", "Sending online notification to HA", 5000,
                                out_should_rotate);
}

esp_err_t ha_notify_offline(void)
{
    return ha_send_notification("offline", "Sending offline notification to HA", 3000, NULL);
}

esp_err_t ha_notify_update(void)
{
    return ha_send_notification("update", "Sending update notification to HA", 5000, NULL);
}
