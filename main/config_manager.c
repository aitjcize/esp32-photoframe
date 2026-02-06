#include "config_manager.h"

#include <string.h>
#include <time.h>

#include "board_hal.h"
#include "config.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "config_manager";

// General
static char device_name[DEVICE_NAME_MAX_LEN] = {0};
static char tz_string[TIMEZONE_MAX_LEN] = {0};
static display_orientation_t display_orientation = DISPLAY_ORIENTATION_LANDSCAPE;
static int display_rotation_deg = BOARD_HAL_DISPLAY_ROTATION_DEG;
static char wifi_ssid[WIFI_SSID_MAX_LEN] = {0};
static char wifi_password[WIFI_PASS_MAX_LEN] = {0};

// Auto Rotate
static bool auto_rotate_enabled = false;
static int rotate_interval = IMAGE_ROTATE_INTERVAL_SEC;
static bool auto_rotate_aligned = true;
static bool sleep_schedule_enabled = false;
static int sleep_schedule_start = 1380;  // Minutes since midnight (23:00 = 23*60)
static int sleep_schedule_end = 420;     // Minutes since midnight (07:00 = 7*60)

#ifdef CONFIG_HAS_SDCARD
static rotation_mode_t rotation_mode = ROTATION_MODE_SDCARD;
#else
static rotation_mode_t rotation_mode = ROTATION_MODE_URL;
#endif

// Auto Rotate - SDCARD
#ifdef CONFIG_HAS_SDCARD
static sd_rotation_mode_t sd_rotation_mode = SD_ROTATION_RANDOM;
static int32_t last_index = -1;
#else
static sd_rotation_mode_t sd_rotation_mode = SD_ROTATION_RANDOM;
static int32_t last_index = -1;
#endif

// Auto Rotate - URL
static char image_url[IMAGE_URL_MAX_LEN] = {0};
static char access_token[ACCESS_TOKEN_MAX_LEN] = {0};
static char http_header_key[HTTP_HEADER_KEY_MAX_LEN] = {0};
static char http_header_value[HTTP_HEADER_VALUE_MAX_LEN] = {0};
static bool save_downloaded_images = true;

// Home Assistant
static char ha_url[HA_URL_MAX_LEN] = {0};

// AI Generation
static char openai_api_key[AI_API_KEY_MAX_LEN] = {0};
static char google_api_key[AI_API_KEY_MAX_LEN] = {0};
static char ai_prompt[AI_PROMPT_MAX_LEN] = {0};
static char ai_model[AI_MODEL_MAX_LEN] = {0};
static ai_provider_t ai_provider = AI_PROVIDER_OPENAI;

esp_err_t config_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing config manager");

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        // General
        size_t device_name_len = DEVICE_NAME_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_DEVICE_NAME_KEY, device_name, &device_name_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded device name from NVS: %s", device_name);
        } else {
            strncpy(device_name, DEFAULT_DEVICE_NAME, DEVICE_NAME_MAX_LEN - 1);
            device_name[DEVICE_NAME_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No device name in NVS, using default: %s", device_name);
        }

        size_t tz_len = TIMEZONE_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_TIMEZONE_KEY, tz_string, &tz_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded timezone from NVS: %s", tz_string);
        } else {
            strncpy(tz_string, DEFAULT_TIMEZONE, TIMEZONE_MAX_LEN - 1);
            tz_string[TIMEZONE_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No timezone in NVS, using default: %s", tz_string);
        }

        uint8_t stored_orientation = DISPLAY_ORIENTATION_LANDSCAPE;
        if (nvs_get_u8(nvs_handle, NVS_DISPLAY_ORIENTATION_KEY, &stored_orientation) == ESP_OK) {
            display_orientation = (display_orientation_t) stored_orientation;
            ESP_LOGI(
                TAG, "Loaded display orientation from NVS: %s",
                display_orientation == DISPLAY_ORIENTATION_LANDSCAPE ? "landscape" : "portrait");
        }

        int32_t stored_display_rotation_deg = 0;
        if (nvs_get_i32(nvs_handle, NVS_DISPLAY_ROTATION_DEG_KEY, &stored_display_rotation_deg) ==
            ESP_OK) {
            display_rotation_deg = stored_display_rotation_deg;
            ESP_LOGI(TAG, "Loaded display rotation from NVS: %d degrees", display_rotation_deg);
        }

        size_t wifi_ssid_len = WIFI_SSID_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_WIFI_SSID_KEY, wifi_ssid, &wifi_ssid_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded WiFi SSID from NVS: %s", wifi_ssid);
        } else {
            strncpy(wifi_ssid, DEFAULT_WIFI_SSID, WIFI_SSID_MAX_LEN - 1);
            wifi_ssid[WIFI_SSID_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No WiFi SSID in NVS, using default: %s", wifi_ssid);
        }

        size_t wifi_pass_len = WIFI_PASS_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_WIFI_PASS_KEY, wifi_password, &wifi_pass_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded WiFi password from NVS (length: %zu)", wifi_pass_len);
        } else {
            strncpy(wifi_password, DEFAULT_WIFI_PASSWORD, WIFI_PASS_MAX_LEN - 1);
            wifi_password[WIFI_PASS_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No WiFi password in NVS, using default");
        }

        // Auto Rotate
        uint8_t stored_enabled = 0;
        if (nvs_get_u8(nvs_handle, NVS_AUTO_ROTATE_KEY, &stored_enabled) == ESP_OK) {
            auto_rotate_enabled = (stored_enabled != 0);
            ESP_LOGI(TAG, "Loaded auto-rotate enabled from NVS: %s",
                     auto_rotate_enabled ? "yes" : "no");
        }

        int32_t stored_interval = IMAGE_ROTATE_INTERVAL_SEC;
        if (nvs_get_i32(nvs_handle, NVS_ROTATE_INTERVAL_KEY, &stored_interval) == ESP_OK) {
            rotate_interval = stored_interval;
            ESP_LOGI(TAG, "Loaded rotate interval from NVS: %d seconds", rotate_interval);
        }

        uint8_t stored_aligned = 1;
        if (nvs_get_u8(nvs_handle, NVS_AUTO_ROTATE_ALIGNED_KEY, &stored_aligned) == ESP_OK) {
            auto_rotate_aligned = (stored_aligned != 0);
            ESP_LOGI(TAG, "Loaded auto-rotate aligned from NVS: %s",
                     auto_rotate_aligned ? "yes" : "no");
        }

        uint8_t stored_sleep_sched_enabled = 0;
        if (nvs_get_u8(nvs_handle, NVS_SLEEP_SCHEDULE_ENABLED_KEY, &stored_sleep_sched_enabled) ==
            ESP_OK) {
            sleep_schedule_enabled = (stored_sleep_sched_enabled != 0);
            ESP_LOGI(TAG, "Loaded sleep schedule enabled from NVS: %s",
                     sleep_schedule_enabled ? "yes" : "no");
        }

        int32_t stored_start = 1380;  // Default 23:00
        if (nvs_get_i32(nvs_handle, NVS_SLEEP_SCHEDULE_START_KEY, &stored_start) == ESP_OK) {
            sleep_schedule_start = stored_start;
            ESP_LOGI(TAG, "Loaded sleep schedule start from NVS: %d minutes (%02d:%02d)",
                     sleep_schedule_start, sleep_schedule_start / 60, sleep_schedule_start % 60);
        }

        int32_t stored_end = 420;  // Default 07:00
        if (nvs_get_i32(nvs_handle, NVS_SLEEP_SCHEDULE_END_KEY, &stored_end) == ESP_OK) {
            sleep_schedule_end = stored_end;
            ESP_LOGI(TAG, "Loaded sleep schedule end from NVS: %d minutes (%02d:%02d)",
                     sleep_schedule_end, sleep_schedule_end / 60, sleep_schedule_end % 60);
        }

        uint8_t stored_mode = ROTATION_MODE_SDCARD;
        if (nvs_get_u8(nvs_handle, NVS_ROTATION_MODE_KEY, &stored_mode) == ESP_OK) {
            rotation_mode = (rotation_mode_t) stored_mode;
#ifndef CONFIG_HAS_SDCARD
            if (rotation_mode == ROTATION_MODE_SDCARD) {
                rotation_mode = ROTATION_MODE_URL;
            }
#endif
            ESP_LOGI(TAG, "Loaded rotation mode from NVS: %s",
                     rotation_mode == ROTATION_MODE_URL ? "url" : "sdcard");
        }

        // Auto Rotate - SDCARD
        uint8_t stored_sd_mode = SD_ROTATION_RANDOM;
        if (nvs_get_u8(nvs_handle, NVS_SD_ROTATION_MODE_KEY, &stored_sd_mode) == ESP_OK) {
            sd_rotation_mode = (sd_rotation_mode_t) stored_sd_mode;
            ESP_LOGI(TAG, "Loaded SD rotation mode from NVS: %s",
                     sd_rotation_mode == SD_ROTATION_SEQUENTIAL ? "sequential" : "random");
        }

        int32_t stored_last_index = -1;
        if (nvs_get_i32(nvs_handle, NVS_LAST_INDEX_KEY, &stored_last_index) == ESP_OK) {
            last_index = stored_last_index;
            ESP_LOGI(TAG, "Loaded last index from NVS: %ld", (long) last_index);
        }

        // Auto Rotate - URL
        size_t url_len = IMAGE_URL_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_IMAGE_URL_KEY, image_url, &url_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded image URL from NVS: %s", image_url);
        } else {
            strncpy(image_url, DEFAULT_IMAGE_URL, IMAGE_URL_MAX_LEN - 1);
            image_url[IMAGE_URL_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No image URL in NVS, using default: %s", image_url);
        }

        size_t access_token_len = ACCESS_TOKEN_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_ACCESS_TOKEN_KEY, access_token, &access_token_len) ==
            ESP_OK) {
            ESP_LOGI(TAG, "Loaded access token from NVS (length: %zu)", access_token_len);
        }

        size_t http_header_key_len = HTTP_HEADER_KEY_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_HTTP_HEADER_KEY_KEY, http_header_key,
                        &http_header_key_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded HTTP header key from NVS: %s", http_header_key);
        }

        size_t http_header_value_len = HTTP_HEADER_VALUE_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_HTTP_HEADER_VALUE_KEY, http_header_value,
                        &http_header_value_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded HTTP header value from NVS (length: %zu)", http_header_value_len);
        }

        uint8_t stored_save_dl = 1;
        if (nvs_get_u8(nvs_handle, NVS_SAVE_DOWNLOADED_KEY, &stored_save_dl) == ESP_OK) {
            save_downloaded_images = (stored_save_dl != 0);
            ESP_LOGI(TAG, "Loaded save_downloaded_images from NVS: %s",
                     save_downloaded_images ? "yes" : "no");
        }

        // Home Assistant
        size_t ha_url_len = HA_URL_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_HA_URL_KEY, ha_url, &ha_url_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded HA URL from NVS: %s", ha_url);
        } else {
            strncpy(ha_url, DEFAULT_HA_URL, HA_URL_MAX_LEN - 1);
            ha_url[HA_URL_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No HA URL in NVS, using default (empty)");
        }

        // AI Generation
        size_t openai_key_len = AI_API_KEY_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_OPENAI_API_KEY_KEY, openai_api_key, &openai_key_len) ==
            ESP_OK) {
            ESP_LOGI(TAG, "Loaded OpenAI API Key from NVS");
        }

        size_t google_key_len = AI_API_KEY_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_GOOGLE_API_KEY_KEY, google_api_key, &google_key_len) ==
            ESP_OK) {
            ESP_LOGI(TAG, "Loaded Google API Key from NVS");
        }

        size_t ai_prompt_len = AI_PROMPT_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_AI_PROMPT_KEY, ai_prompt, &ai_prompt_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded AI prompt from NVS");
        }

        uint8_t stored_provider = AI_PROVIDER_OPENAI;
        if (nvs_get_u8(nvs_handle, NVS_AI_PROVIDER_KEY, &stored_provider) == ESP_OK) {
            ai_provider = (ai_provider_t) stored_provider;
            ESP_LOGI(TAG, "Loaded AI provider from NVS: %s",
                     ai_provider == AI_PROVIDER_OPENAI ? "OpenAI" : "Google");
        }

        size_t ai_model_len = AI_MODEL_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_AI_MODEL_KEY, ai_model, &ai_model_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded AI model from NVS: %s", ai_model);
        } else {
            strncpy(ai_model, DEFAULT_AI_MODEL, AI_MODEL_MAX_LEN - 1);
            ai_model[AI_MODEL_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No AI model in NVS, using default: %s", ai_model);
        }

        nvs_close(nvs_handle);
    }

    // Apply timezone setting
    setenv("TZ", tz_string, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to: %s", tz_string);

    // Log current system time in local timezone
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);

    // Calculate UTC offset for display
    struct tm utc_timeinfo;
    gmtime_r(&now, &utc_timeinfo);
    int offset_hours = timeinfo.tm_hour - utc_timeinfo.tm_hour;

    // Handle day boundary crossing
    if (offset_hours > 12)
        offset_hours -= 24;
    if (offset_hours < -12)
        offset_hours += 24;

    ESP_LOGI(TAG, "Config manager initialized");
    return ESP_OK;
}
// ============================================================================
// General
// ============================================================================

void config_manager_set_device_name(const char *name)
{
    if (name == NULL) {
        return;
    }

    strncpy(device_name, name, DEVICE_NAME_MAX_LEN - 1);
    device_name[DEVICE_NAME_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_DEVICE_NAME_KEY, device_name);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Device name set to: %s", device_name);
}
const char *config_manager_get_device_name(void)
{
    return device_name;
}

void config_manager_set_timezone(const char *tz)
{
    if (tz == NULL) {
        return;
    }

    strncpy(tz_string, tz, TIMEZONE_MAX_LEN - 1);
    tz_string[TIMEZONE_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_TIMEZONE_KEY, tz_string);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    // Apply timezone immediately
    setenv("TZ", tz_string, 1);
    tzset();

    ESP_LOGI(TAG, "Timezone set to: %s", tz_string);
}
const char *config_manager_get_timezone(void)
{
    if (tz_string[0] == '\0') {
        return "UTC0";
    }
    return tz_string;
}

void config_manager_set_display_orientation(display_orientation_t orientation)
{
    display_orientation = orientation;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_DISPLAY_ORIENTATION_KEY, (uint8_t) orientation);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Display orientation set to: %s",
             orientation == DISPLAY_ORIENTATION_LANDSCAPE ? "landscape" : "portrait");
}

display_orientation_t config_manager_get_display_orientation(void)
{
    return display_orientation;
}

void config_manager_set_display_rotation_deg(int rotation_deg)
{
    display_rotation_deg = rotation_deg;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_DISPLAY_ROTATION_DEG_KEY, rotation_deg);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Display rotation set to %d degrees", rotation_deg);
}

int config_manager_get_display_rotation_deg(void)
{
    return display_rotation_deg;
}

void config_manager_set_wifi_ssid(const char *ssid)
{
    if (ssid == NULL) {
        return;
    }

    strncpy(wifi_ssid, ssid, WIFI_SSID_MAX_LEN - 1);
    wifi_ssid[WIFI_SSID_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_WIFI_SSID_KEY, wifi_ssid);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "WiFi SSID set to: %s", wifi_ssid);
}

const char *config_manager_get_wifi_ssid(void)
{
    return wifi_ssid;
}

void config_manager_set_wifi_password(const char *password)
{
    if (password == NULL) {
        return;
    }

    strncpy(wifi_password, password, WIFI_PASS_MAX_LEN - 1);
    wifi_password[WIFI_PASS_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_WIFI_PASS_KEY, wifi_password);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "WiFi password set (length: %zu)", strlen(wifi_password));
}

const char *config_manager_get_wifi_password(void)
{
    return wifi_password;
}
// ============================================================================
// Auto Rotate
// ============================================================================

void config_manager_set_auto_rotate(bool enabled)
{
    auto_rotate_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_AUTO_ROTATE_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Auto-rotate %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_auto_rotate(void)
{
    return auto_rotate_enabled;
}

void config_manager_set_rotate_interval(int seconds)
{
    rotate_interval = seconds;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_ROTATE_INTERVAL_KEY, seconds);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Rotate interval set to %d seconds", seconds);
}

int config_manager_get_rotate_interval(void)
{
    return rotate_interval;
}

void config_manager_set_auto_rotate_aligned(bool enabled)
{
    auto_rotate_aligned = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_AUTO_ROTATE_ALIGNED_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Auto-rotate aligned %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_auto_rotate_aligned(void)
{
    return auto_rotate_aligned;
}

void config_manager_set_sleep_schedule_enabled(bool enabled)
{
    sleep_schedule_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_SLEEP_SCHEDULE_ENABLED_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Sleep schedule %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_sleep_schedule_enabled(void)
{
    return sleep_schedule_enabled;
}

void config_manager_set_sleep_schedule_start(int minutes)
{
    sleep_schedule_start = minutes;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_SLEEP_SCHEDULE_START_KEY, minutes);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Sleep schedule start set to: %d minutes (%02d:%02d)", minutes, minutes / 60,
             minutes % 60);
}

int config_manager_get_sleep_schedule_start(void)
{
    return sleep_schedule_start;
}

void config_manager_set_sleep_schedule_end(int minutes)
{
    sleep_schedule_end = minutes;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_SLEEP_SCHEDULE_END_KEY, minutes);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Sleep schedule end set to: %d minutes (%02d:%02d)", minutes, minutes / 60,
             minutes % 60);
}

int config_manager_get_sleep_schedule_end(void)
{
    return sleep_schedule_end;
}

bool config_manager_is_in_sleep_schedule(void)
{
    if (!sleep_schedule_enabled) {
        return false;
    }

    // Get current time
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    int current_minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;

    // Handle overnight schedules (e.g., 23:00 - 07:00)
    if (sleep_schedule_start > sleep_schedule_end) {
        // Schedule crosses midnight
        return current_minutes >= sleep_schedule_start || current_minutes < sleep_schedule_end;
    } else {
        // Schedule within same day
        return current_minutes >= sleep_schedule_start && current_minutes < sleep_schedule_end;
    }
}

void config_manager_set_rotation_mode(rotation_mode_t mode)
{
#ifndef CONFIG_HAS_SDCARD
    if (mode == ROTATION_MODE_SDCARD) {
        ESP_LOGE(TAG, "Cannot set rotation mode to SDCARD: SD card not supported");
        return;
    }
#endif
    rotation_mode = mode;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_ROTATION_MODE_KEY, (uint8_t) mode);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Rotation mode set to: %s", mode == ROTATION_MODE_URL ? "url" : "sdcard");
}

rotation_mode_t config_manager_get_rotation_mode(void)
{
    return rotation_mode;
}
// ============================================================================
// Auto Rotate - SDCARD
// ============================================================================

void config_manager_set_sd_rotation_mode(sd_rotation_mode_t mode)
{
    sd_rotation_mode = mode;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_SD_ROTATION_MODE_KEY, (uint8_t) mode);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "SD rotation mode set to: %s",
             mode == SD_ROTATION_SEQUENTIAL ? "sequential" : "random");
}

sd_rotation_mode_t config_manager_get_sd_rotation_mode(void)
{
    return sd_rotation_mode;
}

void config_manager_set_last_index(int32_t index)
{
    last_index = index;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_LAST_INDEX_KEY, index);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

int32_t config_manager_get_last_index(void)
{
    return last_index;
}
// ============================================================================
// Auto Rotate - URL
// ============================================================================

void config_manager_set_image_url(const char *url)
{
    if (url) {
        strncpy(image_url, url, IMAGE_URL_MAX_LEN - 1);
        image_url[IMAGE_URL_MAX_LEN - 1] = '\0';
    } else {
        image_url[0] = '\0';
    }

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        if (image_url[0] != '\0') {
            nvs_set_str(nvs_handle, NVS_IMAGE_URL_KEY, image_url);
        } else {
            nvs_erase_key(nvs_handle, NVS_IMAGE_URL_KEY);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Image URL set to: %s", image_url[0] ? image_url : "(empty)");
}
const char *config_manager_get_image_url(void)
{
    return image_url;
}

void config_manager_set_access_token(const char *token)
{
    if (token == NULL) {
        return;
    }

    strncpy(access_token, token, ACCESS_TOKEN_MAX_LEN - 1);
    access_token[ACCESS_TOKEN_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_ACCESS_TOKEN_KEY, access_token);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Access token set (length: %zu)", strlen(access_token));
}
const char *config_manager_get_access_token(void)
{
    return access_token;
}

void config_manager_set_http_header_key(const char *key)
{
    if (key == NULL) {
        return;
    }

    strncpy(http_header_key, key, HTTP_HEADER_KEY_MAX_LEN - 1);
    http_header_key[HTTP_HEADER_KEY_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_HTTP_HEADER_KEY_KEY, http_header_key);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "HTTP header key set to: %s", http_header_key);
}
const char *config_manager_get_http_header_key(void)
{
    return http_header_key;
}

void config_manager_set_http_header_value(const char *value)
{
    if (value == NULL) {
        return;
    }

    strncpy(http_header_value, value, HTTP_HEADER_VALUE_MAX_LEN - 1);
    http_header_value[HTTP_HEADER_VALUE_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_HTTP_HEADER_VALUE_KEY, http_header_value);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "HTTP header value set (length: %zu)", strlen(http_header_value));
}
const char *config_manager_get_http_header_value(void)
{
    return http_header_value;
}

void config_manager_set_save_downloaded_images(bool enabled)
{
    save_downloaded_images = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_SAVE_DOWNLOADED_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Save downloaded images %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_save_downloaded_images(void)
{
    return save_downloaded_images;
}
// ============================================================================
// Home Assistant
// ============================================================================

void config_manager_set_ha_url(const char *url)
{
    if (url) {
        strncpy(ha_url, url, HA_URL_MAX_LEN - 1);
        ha_url[HA_URL_MAX_LEN - 1] = '\0';

        nvs_handle_t nvs_handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
            nvs_set_str(nvs_handle, NVS_HA_URL_KEY, ha_url);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }

        ESP_LOGI(TAG, "HA URL set to: %s", ha_url);
    }
}
const char *config_manager_get_ha_url(void)
{
    return ha_url;
}

// ============================================================================
// AI Generation
// ============================================================================

void config_manager_set_openai_api_key(const char *key)
{
    if (key == NULL) {
        return;
    }

    strncpy(openai_api_key, key, AI_API_KEY_MAX_LEN - 1);
    openai_api_key[AI_API_KEY_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_OPENAI_API_KEY_KEY, openai_api_key);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "OpenAI API Key set");
}

const char *config_manager_get_openai_api_key(void)
{
    return openai_api_key;
}

void config_manager_set_google_api_key(const char *key)
{
    if (key == NULL) {
        return;
    }

    strncpy(google_api_key, key, AI_API_KEY_MAX_LEN - 1);
    google_api_key[AI_API_KEY_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_GOOGLE_API_KEY_KEY, google_api_key);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Google API Key set");
}

const char *config_manager_get_google_api_key(void)
{
    return google_api_key;
}

void config_manager_set_ai_prompt(const char *prompt)
{
    if (prompt == NULL) {
        return;
    }

    strncpy(ai_prompt, prompt, AI_PROMPT_MAX_LEN - 1);
    ai_prompt[AI_PROMPT_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_AI_PROMPT_KEY, ai_prompt);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "AI prompt set: %s", ai_prompt);
}

const char *config_manager_get_ai_prompt(void)
{
    return ai_prompt;
}

void config_manager_set_ai_provider(ai_provider_t provider)
{
    ai_provider = provider;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_AI_PROVIDER_KEY, (uint8_t) provider);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "AI provider set to: %s", provider == AI_PROVIDER_OPENAI ? "OpenAI" : "Google");
}

ai_provider_t config_manager_get_ai_provider(void)
{
    return ai_provider;
}

void config_manager_set_ai_model(const char *model)
{
    if (model == NULL) {
        return;
    }

    strncpy(ai_model, model, AI_MODEL_MAX_LEN - 1);
    ai_model[AI_MODEL_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_AI_MODEL_KEY, ai_model);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "AI model set to: %s", ai_model);
}

const char *config_manager_get_ai_model(void)
{
    if (ai_model[0] == '\0') {
        return DEFAULT_AI_MODEL;
    }
    return ai_model;
}