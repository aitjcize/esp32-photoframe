#include "ota_manager.h"

#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "power_manager.h"

static const char *TAG = "ota_manager";

static ota_status_t ota_status = {.state = OTA_STATE_IDLE,
                                  .current_version = "",
                                  .latest_version = "",
                                  .error_message = "",
                                  .progress_percent = 0};

static SemaphoreHandle_t ota_status_mutex = NULL;
static esp_timer_handle_t ota_check_timer = NULL;
static bool update_available = false;
static char firmware_url[256] = "";

static void set_ota_state(ota_state_t state, const char *error_msg)
{
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        ota_status.state = state;
        if (error_msg) {
            strncpy(ota_status.error_message, error_msg, sizeof(ota_status.error_message) - 1);
        } else {
            ota_status.error_message[0] = '\0';
        }
        xSemaphoreGive(ota_status_mutex);
    }
}

static int version_compare(const char *v1, const char *v2)
{
    // Simple version comparison
    // Handles formats like "v1.2.3" or "1.2.3" or "dev-abc123"

    // Skip 'v' prefix if present
    if (v1[0] == 'v')
        v1++;
    if (v2[0] == 'v')
        v2++;

    // Dev versions are always considered older than release versions
    bool v1_is_dev = (strncmp(v1, "dev-", 4) == 0);
    bool v2_is_dev = (strncmp(v2, "dev-", 4) == 0);

    if (v1_is_dev && !v2_is_dev) {
        return -1;  // v1 (dev) is older than v2 (release)
    }
    if (!v1_is_dev && v2_is_dev) {
        return 1;  // v1 (release) is newer than v2 (dev)
    }
    if (v1_is_dev && v2_is_dev) {
        return strcmp(v1, v2);  // Both dev, compare strings
    }

    // Parse version numbers for release versions
    int v1_major = 0, v1_minor = 0, v1_patch = 0;
    int v2_major = 0, v2_minor = 0, v2_patch = 0;

    sscanf(v1, "%d.%d.%d", &v1_major, &v1_minor, &v1_patch);
    sscanf(v2, "%d.%d.%d", &v2_major, &v2_minor, &v2_patch);

    if (v1_major != v2_major)
        return v1_major - v2_major;
    if (v1_minor != v2_minor)
        return v1_minor - v2_minor;
    return v1_patch - v2_patch;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    case HTTP_EVENT_ON_HEADERS_COMPLETE:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADERS_COMPLETE");
        break;
    case HTTP_EVENT_ON_STATUS_CODE:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_STATUS_CODE");
        break;
    }
    return ESP_OK;
}

static esp_err_t fetch_github_release_info(char *latest_version, size_t version_len,
                                           char *download_url, size_t url_len)
{
    esp_err_t err = ESP_FAIL;
    char *response_buffer = NULL;
    int response_len = 0;

    esp_http_client_config_t config = {
        .url = GITHUB_API_URL,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    // Set User-Agent header (GitHub API requires it)
    esp_http_client_set_header(client, "User-Agent", "ESP32-PhotoFrame");

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        goto cleanup;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed, status = %d", status_code);
        err = ESP_FAIL;
        goto cleanup;
    }

    if (content_length <= 0) {
        ESP_LOGE(TAG, "Invalid content length: %d", content_length);
        err = ESP_FAIL;
        goto cleanup;
    }

    response_buffer = malloc(content_length + 1);
    if (response_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for response");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    response_len = esp_http_client_read_response(client, response_buffer, content_length);
    if (response_len <= 0) {
        ESP_LOGE(TAG, "Failed to read response");
        err = ESP_FAIL;
        goto cleanup;
    }

    response_buffer[response_len] = '\0';

    // Parse JSON response
    cJSON *json = cJSON_Parse(response_buffer);
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        err = ESP_FAIL;
        goto cleanup;
    }

    // Get tag_name (version)
    cJSON *tag_name = cJSON_GetObjectItem(json, "tag_name");
    if (tag_name == NULL || !cJSON_IsString(tag_name)) {
        ESP_LOGE(TAG, "tag_name not found in response");
        cJSON_Delete(json);
        err = ESP_FAIL;
        goto cleanup;
    }

    strncpy(latest_version, tag_name->valuestring, version_len - 1);
    latest_version[version_len - 1] = '\0';

    // Get assets array and find .bin file
    cJSON *assets = cJSON_GetObjectItem(json, "assets");
    if (assets == NULL || !cJSON_IsArray(assets)) {
        ESP_LOGE(TAG, "assets not found in response");
        cJSON_Delete(json);
        err = ESP_FAIL;
        goto cleanup;
    }

    bool found_binary = false;
    cJSON *asset = NULL;
    cJSON_ArrayForEach(asset, assets)
    {
        cJSON *name = cJSON_GetObjectItem(asset, "name");
        if (name && cJSON_IsString(name)) {
            const char *asset_name = name->valuestring;
            // Look for esp32-photoframe.bin specifically (the OTA firmware binary)
            if (strstr(asset_name, "esp32-photoframe.bin") != NULL) {
                cJSON *browser_download_url = cJSON_GetObjectItem(asset, "browser_download_url");
                if (browser_download_url && cJSON_IsString(browser_download_url)) {
                    strncpy(download_url, browser_download_url->valuestring, url_len - 1);
                    download_url[url_len - 1] = '\0';
                    found_binary = true;
                    ESP_LOGI(TAG, "Found firmware binary: %s", asset_name);
                    break;
                }
            }
        }
    }

    cJSON_Delete(json);

    if (!found_binary) {
        ESP_LOGE(TAG, "No .bin file found in release assets");
        err = ESP_FAIL;
        goto cleanup;
    }

    err = ESP_OK;
    ESP_LOGI(TAG, "Latest version: %s", latest_version);
    ESP_LOGI(TAG, "Download URL: %s", download_url);

cleanup:
    if (response_buffer) {
        free(response_buffer);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return err;
}

static void ota_check_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Checking for firmware updates...");

    set_ota_state(OTA_STATE_CHECKING, NULL);

    char latest_version[32] = {0};
    char download_url[256] = {0};

    esp_err_t err = fetch_github_release_info(latest_version, sizeof(latest_version), download_url,
                                              sizeof(download_url));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to fetch release info");
        set_ota_state(OTA_STATE_ERROR, "Failed to check for updates");
        vTaskDelete(NULL);
        return;
    }

    // Store latest version and URL
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        strncpy(ota_status.latest_version, latest_version, sizeof(ota_status.latest_version) - 1);
        xSemaphoreGive(ota_status_mutex);
    }
    strncpy(firmware_url, download_url, sizeof(firmware_url) - 1);

    // Compare versions
    int cmp = version_compare(ota_status.current_version, latest_version);

    if (cmp < 0) {
        ESP_LOGI(TAG, "Update available: %s -> %s", ota_status.current_version, latest_version);
        update_available = true;
        set_ota_state(OTA_STATE_UPDATE_AVAILABLE, NULL);
    } else {
        ESP_LOGI(TAG, "Already on latest version: %s", ota_status.current_version);
        update_available = false;
        set_ota_state(OTA_STATE_IDLE, NULL);
    }

    vTaskDelete(NULL);
}

static void ota_update_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Starting OTA update...");

    // Reset sleep timer to prevent auto-sleep during OTA
    power_manager_reset_sleep_timer();

    set_ota_state(OTA_STATE_DOWNLOADING, NULL);
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        ota_status.progress_percent = 0;
        xSemaphoreGive(ota_status_mutex);
    }

    esp_http_client_config_t config = {
        .url = firmware_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .buffer_size = 8192,
        .buffer_size_tx = 4096,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        set_ota_state(OTA_STATE_ERROR, "Failed to start OTA update");
        vTaskDelete(NULL);
        return;
    }

    int image_size = esp_https_ota_get_image_size(https_ota_handle);
    ESP_LOGI(TAG, "OTA image size: %d bytes", image_size);

    set_ota_state(OTA_STATE_INSTALLING, NULL);

    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        int downloaded = esp_https_ota_get_image_len_read(https_ota_handle);
        if (image_size > 0) {
            int progress = (downloaded * 100) / image_size;
            if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
                ota_status.progress_percent = progress;
                xSemaphoreGive(ota_status_mutex);
            }
            ESP_LOGI(TAG, "OTA progress: %d%%", progress);
        }

        // Reset sleep timer periodically during OTA to prevent auto-sleep
        power_manager_reset_sleep_timer();

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(https_ota_handle);
        set_ota_state(OTA_STATE_ERROR, "OTA update failed");
        vTaskDelete(NULL);
        return;
    }

    err = esp_https_ota_finish(https_ota_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed");
            set_ota_state(OTA_STATE_ERROR, "Firmware validation failed");
        } else {
            ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
            set_ota_state(OTA_STATE_ERROR, "Failed to finalize OTA update");
        }
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA update successful! Rebooting in 3 seconds...");
    set_ota_state(OTA_STATE_SUCCESS, NULL);
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        ota_status.progress_percent = 100;
        xSemaphoreGive(ota_status_mutex);
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();

    vTaskDelete(NULL);
}

static void ota_check_timer_callback(void *arg)
{
    ESP_LOGI(TAG, "Periodic OTA check triggered");
    xTaskCreate(&ota_check_task, "ota_check_task", 12288, NULL, 5, NULL);
}

esp_err_t ota_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing OTA manager");

    // Create mutex for ota_status protection
    ota_status_mutex = xSemaphoreCreateMutex();
    if (ota_status_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create OTA status mutex");
        return ESP_ERR_NO_MEM;
    }

    // Get current firmware version
    const esp_app_desc_t *app_desc = esp_app_get_description();
    strncpy(ota_status.current_version, app_desc->version, sizeof(ota_status.current_version) - 1);

    ESP_LOGI(TAG, "Current firmware version: %s", ota_status.current_version);

    // Mark current partition as valid (for rollback support)
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "First boot after OTA update, marking as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    // Create periodic timer for update checks (24 hours)
    const esp_timer_create_args_t timer_args = {.callback = &ota_check_timer_callback,
                                                .name = "ota_check_timer"};

    esp_err_t err = esp_timer_create(&timer_args, &ota_check_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create OTA check timer: %s", esp_err_to_name(err));
        return err;
    }

    // Start periodic timer (convert milliseconds to microseconds)
    err = esp_timer_start_periodic(ota_check_timer, (uint64_t) OTA_CHECK_INTERVAL_MS * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OTA check timer: %s", esp_err_to_name(err));
        return err;
    }

    // Perform initial check on startup after a delay to allow network to stabilize
    ESP_LOGI(TAG, "Initial OTA check will run in 10 seconds");
    vTaskDelay(pdMS_TO_TICKS(10000));
    xTaskCreate(&ota_check_task, "ota_check_task", 12288, NULL, 5, NULL);

    return ESP_OK;
}

esp_err_t ota_check_for_update(bool *update_available_out)
{
    if (ota_status.state == OTA_STATE_CHECKING || ota_status.state == OTA_STATE_DOWNLOADING ||
        ota_status.state == OTA_STATE_INSTALLING) {
        return ESP_ERR_INVALID_STATE;
    }

    update_available = false;
    xTaskCreate(&ota_check_task, "ota_check_task", 12288, NULL, 5, NULL);

    // Wait for check to complete (with timeout)
    int timeout = 30;  // 30 seconds
    while (timeout > 0 && ota_status.state == OTA_STATE_CHECKING) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        timeout--;
    }

    if (update_available_out) {
        *update_available_out = update_available;
    }

    return ESP_OK;
}

esp_err_t ota_start_update(void)
{
    if (!update_available) {
        ESP_LOGW(TAG, "No update available");
        return ESP_ERR_INVALID_STATE;
    }

    if (ota_status.state == OTA_STATE_DOWNLOADING || ota_status.state == OTA_STATE_INSTALLING) {
        ESP_LOGW(TAG, "Update already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    xTaskCreate(&ota_update_task, "ota_update_task", 12288, NULL, 5, NULL);

    return ESP_OK;
}

void ota_get_status(ota_status_t *status)
{
    if (status && ota_status_mutex) {
        if (xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
            memcpy(status, &ota_status, sizeof(ota_status_t));
            xSemaphoreGive(ota_status_mutex);
        }
    }
}

const char *ota_get_current_version(void)
{
    return ota_status.current_version;
}
