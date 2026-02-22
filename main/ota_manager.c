#include "ota_manager.h"

#include "esp_idf_version.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "board_hal.h"
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
#include "ha_integration.h"
#include "nvs.h"
#include "periodic_tasks.h"
#include "power_manager.h"

static const char *TAG = "ota_manager";
#define OTA_NVS_NAMESPACE "ota"
#define OTA_NVS_LATEST_VERSION_KEY "latest_ver"
#define OTA_NVS_STATE_KEY "state"
#define OTA_CHECK_INTERVAL_SECONDS (24 * 60 * 60)  // 24 hours

static ota_status_t ota_status = {.state = OTA_STATE_IDLE,
                                  .current_version = "",
                                  .latest_version = "",
                                  .error_message = "",
                                  .progress_percent = 0};

static SemaphoreHandle_t ota_status_mutex = NULL;
static bool update_available = false;
static char firmware_url[256] = "";

// Forward declarations
static void ota_save_status_to_nvs(void);
static void ota_load_status_from_nvs(void);
static esp_err_t ota_check_periodic_callback(void);

static void set_ota_state(ota_state_t state, const char *error_msg)
{
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        ota_status.state = state;
        if (error_msg) {
            strncpy(ota_status.error_message, error_msg, sizeof(ota_status.error_message) - 1);
            ota_status.error_message[sizeof(ota_status.error_message) - 1] = '\0';
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
#ifdef HTTP_EVENT_ON_HEADERS_COMPLETE
    case HTTP_EVENT_ON_HEADERS_COMPLETE:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADERS_COMPLETE");
        break;
#endif
#ifdef HTTP_EVENT_ON_STATUS_CODE
    case HTTP_EVENT_ON_STATUS_CODE:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_STATUS_CODE");
        break;
#endif
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

    response_buffer = heap_caps_malloc(content_length + 1, MALLOC_CAP_SPIRAM);
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

    const char *board_name = BOARD_HAL_NAME;

    char target_binary[64];
    snprintf(target_binary, sizeof(target_binary), "esp32-photoframe-%s.bin", board_name);
    ESP_LOGI(TAG, "Searching for board-specific OTA binary: %s", target_binary);

    cJSON_ArrayForEach(asset, assets)
    {
        cJSON *name = cJSON_GetObjectItem(asset, "name");
        if (name && cJSON_IsString(name)) {
            const char *asset_name = name->valuestring;
            // Look for board-specific binary
            if (strcmp(asset_name, target_binary) == 0) {
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
    // pvParameter is a boolean: true = notify HA, false/NULL = don't notify
    bool notify_ha = (pvParameter != NULL);

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
        ota_status.latest_version[sizeof(ota_status.latest_version) - 1] = '\0';
        xSemaphoreGive(ota_status_mutex);
    }
    strncpy(firmware_url, download_url, sizeof(firmware_url) - 1);
    firmware_url[sizeof(firmware_url) - 1] = '\0';

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

    // Update last check time after successful check
    ota_update_last_check_time();

    // Save OTA status to NVS for persistence across reboots
    ota_save_status_to_nvs();

    // Notify HA if requested
    if (notify_ha) {
        ESP_LOGI(TAG, "Notifying HA of OTA status update");
        ha_notify_update();
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

esp_err_t ota_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing OTA manager");

    // Zero out the entire ota_status struct to prevent garbage data
    memset(&ota_status, 0, sizeof(ota_status_t));
    ota_status.state = OTA_STATE_IDLE;

    // Create mutex for ota_status protection
    ota_status_mutex = xSemaphoreCreateMutex();
    if (ota_status_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create OTA status mutex");
        return ESP_ERR_NO_MEM;
    }

    // Get current firmware version
    const esp_app_desc_t *app_desc = esp_app_get_description();
    strncpy(ota_status.current_version, app_desc->version, sizeof(ota_status.current_version) - 1);
    ota_status.current_version[sizeof(ota_status.current_version) - 1] = '\0';

    ESP_LOGI(TAG, "Current firmware version: %s", ota_status.current_version);

    // Load last known OTA status from NVS (latest_version, state)
    ota_load_status_from_nvs();

    // Mark current partition as valid (for rollback support)
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "First boot after OTA update, marking as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    // Register OTA check as a periodic task (24 hours)
    esp_err_t err = periodic_tasks_register(OTA_CHECK_TASK_NAME, ota_check_periodic_callback,
                                            OTA_CHECK_INTERVAL_SECONDS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OTA periodic task: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t ota_check_for_update(bool *update_available_out, int timeout)
{
    if (ota_status.state == OTA_STATE_CHECKING || ota_status.state == OTA_STATE_DOWNLOADING ||
        ota_status.state == OTA_STATE_INSTALLING) {
        return ESP_ERR_INVALID_STATE;
    }

    update_available = false;
    xTaskCreate(&ota_check_task, "ota_check_task", 12288, NULL, 5, NULL);

    // Wait for check to complete (with timeout)
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

bool ota_should_check_daily(void)
{
    return periodic_tasks_should_run(OTA_CHECK_TASK_NAME);
}

void ota_update_last_check_time(void)
{
    periodic_tasks_update_last_run(OTA_CHECK_TASK_NAME);
}

static esp_err_t ota_check_periodic_callback(void)
{
    ESP_LOGI(TAG, "Periodic OTA check triggered");

    // Check for updates without notifying HA (HA will poll for status)
    xTaskCreate(&ota_check_task, "ota_check_task", 12288, NULL, 5, NULL);

    return ESP_OK;
}

static void ota_save_status_to_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for saving OTA status: %s", esp_err_to_name(err));
        return;
    }

    // Save latest_version and state
    if (ota_status_mutex && xSemaphoreTake(ota_status_mutex, portMAX_DELAY) == pdTRUE) {
        err = nvs_set_str(nvs_handle, OTA_NVS_LATEST_VERSION_KEY, ota_status.latest_version);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save latest_version to NVS: %s", esp_err_to_name(err));
        }

        err = nvs_set_u8(nvs_handle, OTA_NVS_STATE_KEY, (uint8_t) ota_status.state);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save state to NVS: %s", esp_err_to_name(err));
        }

        xSemaphoreGive(ota_status_mutex);
    }

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

static void ota_load_status_from_nvs(void)
{
    // Initialize to safe defaults first
    ota_status.latest_version[0] = '\0';
    ota_status.state = OTA_STATE_IDLE;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved OTA status in NVS (first boot or cleared), using defaults");
        return;
    }

    // Load latest_version
    size_t required_size = sizeof(ota_status.latest_version);
    err = nvs_get_str(nvs_handle, OTA_NVS_LATEST_VERSION_KEY, ota_status.latest_version,
                      &required_size);
    if (err != ESP_OK) {
        ota_status.latest_version[0] = '\0';
    }

    // Load state
    uint8_t saved_state = 0;
    err = nvs_get_u8(nvs_handle, OTA_NVS_STATE_KEY, &saved_state);
    if (err == ESP_OK) {
        ota_status.state = (ota_state_t) saved_state;
    } else {
        ota_status.state = OTA_STATE_IDLE;
    }

    nvs_close(nvs_handle);
}
