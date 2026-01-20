#include "config_manager.h"

#include <string.h>
#include <time.h>

#include "config.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "config_manager";

static int rotate_interval = IMAGE_ROTATE_INTERVAL_SEC;
static int image_orientation = IMAGE_ORIENTATION_DEG;
static bool auto_rotate_enabled = false;
static char image_url[IMAGE_URL_MAX_LEN] = {0};
static char ha_url[HA_URL_MAX_LEN] = {0};
static rotation_mode_t rotation_mode = ROTATION_MODE_SDCARD;
static bool save_downloaded_images = true;
static display_orientation_t display_orientation = DISPLAY_ORIENTATION_LANDSCAPE;
static bool sleep_schedule_enabled = false;
static int sleep_schedule_start = 1380;  // Minutes since midnight (23:00 = 23*60)
static int sleep_schedule_end = 420;     // Minutes since midnight (07:00 = 7*60)

esp_err_t config_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing config manager");

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        int32_t stored_interval = IMAGE_ROTATE_INTERVAL_SEC;
        if (nvs_get_i32(nvs_handle, NVS_ROTATE_INTERVAL_KEY, &stored_interval) == ESP_OK) {
            rotate_interval = stored_interval;
            ESP_LOGI(TAG, "Loaded rotate interval from NVS: %d seconds", rotate_interval);
        }

        uint8_t stored_enabled = 0;
        if (nvs_get_u8(nvs_handle, NVS_AUTO_ROTATE_KEY, &stored_enabled) == ESP_OK) {
            auto_rotate_enabled = (stored_enabled != 0);
            ESP_LOGI(TAG, "Loaded auto-rotate enabled from NVS: %s",
                     auto_rotate_enabled ? "yes" : "no");
        }

        int32_t stored_image_orientation = IMAGE_ORIENTATION_DEG;
        if (nvs_get_i32(nvs_handle, NVS_IMAGE_ORIENTATION_KEY, &stored_image_orientation) ==
            ESP_OK) {
            image_orientation = stored_image_orientation;
            ESP_LOGI(TAG, "Loaded image orientation from NVS: %d degrees", image_orientation);
        }

        size_t url_len = IMAGE_URL_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_IMAGE_URL_KEY, image_url, &url_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded image URL from NVS: %s", image_url);
        } else {
            // Set default URL if not configured
            strncpy(image_url, DEFAULT_IMAGE_URL, IMAGE_URL_MAX_LEN - 1);
            image_url[IMAGE_URL_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No image URL in NVS, using default: %s", image_url);
        }

        uint8_t stored_mode = ROTATION_MODE_SDCARD;
        if (nvs_get_u8(nvs_handle, NVS_ROTATION_MODE_KEY, &stored_mode) == ESP_OK) {
            rotation_mode = (rotation_mode_t) stored_mode;
            ESP_LOGI(TAG, "Loaded rotation mode from NVS: %s",
                     rotation_mode == ROTATION_MODE_URL ? "url" : "sdcard");
        }

        uint8_t stored_save_dl = 1;
        if (nvs_get_u8(nvs_handle, NVS_SAVE_DOWNLOADED_KEY, &stored_save_dl) == ESP_OK) {
            save_downloaded_images = (stored_save_dl != 0);
            ESP_LOGI(TAG, "Loaded save_downloaded_images from NVS: %s",
                     save_downloaded_images ? "yes" : "no");
        }

        size_t ha_url_len = HA_URL_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_HA_URL_KEY, ha_url, &ha_url_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded HA URL from NVS: %s", ha_url);
        } else {
            // Set default (empty) if not configured
            strncpy(ha_url, DEFAULT_HA_URL, HA_URL_MAX_LEN - 1);
            ha_url[HA_URL_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No HA URL in NVS, using default (empty)");
        }

        uint8_t stored_orientation = DISPLAY_ORIENTATION_LANDSCAPE;
        if (nvs_get_u8(nvs_handle, NVS_DISPLAY_ORIENTATION_KEY, &stored_orientation) == ESP_OK) {
            display_orientation = (display_orientation_t) stored_orientation;
            ESP_LOGI(
                TAG, "Loaded display orientation from NVS: %s",
                display_orientation == DISPLAY_ORIENTATION_LANDSCAPE ? "landscape" : "portrait");
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

        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Config manager initialized");
    return ESP_OK;
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

void config_manager_set_image_orientation(int orientation)
{
    image_orientation = orientation;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_IMAGE_ORIENTATION_KEY, orientation);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Image orientation set to %d degrees", orientation);
}

int config_manager_get_image_orientation(void)
{
    return image_orientation;
}

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

void config_manager_set_rotation_mode(rotation_mode_t mode)
{
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
