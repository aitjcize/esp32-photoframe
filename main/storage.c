#include "storage.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "config_manager.h"
#include "esp_err.h"
#include "esp_log.h"

#ifdef CONFIG_HAS_SDCARD
#include "sdcard.h"
#endif

#ifdef CONFIG_USE_INTERNAL_FLASH_STORAGE
#include "esp_littlefs.h"
#endif

#include <sys/stat.h>

#include "album_manager.h"
#include "memfs.h"
#include "utils.h"

static const char *TAG = "storage";
static storage_type_t current_storage_type = STORAGE_TYPE_NONE;

#ifdef CONFIG_USE_INTERNAL_FLASH_STORAGE
static esp_err_t mount_littlefs(void)
{
    ESP_LOGI(TAG, "Initializing LittleFS");

    esp_vfs_littlefs_conf_t conf = {
        .base_path = FS_MOUNT_POINT,
        .partition_label = LITTLEFS_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "LittleFS Partition size: total: %d, used: %d", total, used);
    }

    return ESP_OK;
}
#endif

esp_err_t storage_init(void)
{
    esp_err_t ret = ESP_OK;

#ifdef CONFIG_HAS_SDCARD
    // For devices with SD card configured, SD card handles its own init in board_hal_init.
    // We just check if it was successfully mounted.
    if (sdcard_is_mounted()) {
        ESP_LOGI(TAG, "SD Card storage is active");
        current_storage_type = STORAGE_TYPE_SDCARD;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "SD Card not mounted. Attempting fallback...");
#endif

#ifdef CONFIG_USE_INTERNAL_FLASH_STORAGE
    // Fallback or explicit flash storage
    if (mount_littlefs() == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS storage is active");
        current_storage_type = STORAGE_TYPE_LITTLEFS;
        return ESP_OK;
    }
#endif

    // Final fallback to MemFS
    ESP_LOGW(TAG, "No persistent storage available, mounting MemFS at %s", FS_MOUNT_POINT);
    ret = memfs_mount(FS_MOUNT_POINT, 10);
    if (ret == ESP_OK) {
        current_storage_type = STORAGE_TYPE_MEMFS;
    } else {
        ESP_LOGE(TAG, "Failed to mount MemFS fallback!");
    }

    return ret;
}

storage_type_t storage_get_type(void)
{
    return current_storage_type;
}

bool storage_has_persistent_storage(void)
{
    return current_storage_type == STORAGE_TYPE_SDCARD ||
           current_storage_type == STORAGE_TYPE_LITTLEFS;
}

void storage_unmount(void)
{
#ifdef CONFIG_USE_INTERNAL_FLASH_STORAGE
    if (current_storage_type == STORAGE_TYPE_LITTLEFS) {
        ESP_LOGI(TAG, "Unmounting LittleFS before deep sleep");
        esp_vfs_littlefs_unregister(LITTLEFS_PARTITION_LABEL);
    }
#endif
}

esp_err_t storage_format(void)
{
#ifdef CONFIG_USE_INTERNAL_FLASH_STORAGE
    if (current_storage_type != STORAGE_TYPE_LITTLEFS) {
        ESP_LOGE(TAG, "Format only supported for LittleFS storage");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGW(TAG, "Formatting LittleFS partition...");

    // Unmount, format, and remount
    esp_vfs_littlefs_unregister(LITTLEFS_PARTITION_LABEL);
    esp_err_t ret = esp_littlefs_format(LITTLEFS_PARTITION_LABEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to format LittleFS: %s", esp_err_to_name(ret));
        // Try to remount anyway
        mount_littlefs();
        return ret;
    }

    ESP_LOGI(TAG, "LittleFS formatted successfully, remounting...");
    ret = mount_littlefs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to remount LittleFS after format");
        return ret;
    }

    // Recreate the images directory after format
    mkdir(IMAGE_DIRECTORY, 0775);
    album_manager_ensure_default_album();

    ESP_LOGI(TAG, "Storage format complete");
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t storage_read_wifi_credentials(char *ssid, char *password)
{
#ifdef CONFIG_HAS_SDCARD
    if (!sdcard_is_mounted()) {
        return ESP_ERR_NOT_FOUND;
    }

    // Try multiple possible locations, starting with the safest (config folder)
    const char *paths[] = {FS_MOUNT_POINT "/config/wifi.txt", FS_MOUNT_POINT "/wifi.txt", NULL};

    FILE *f = NULL;
    for (int i = 0; paths[i] != NULL; i++) {
        f = fopen(paths[i], "r");
        if (f) {
            ESP_LOGI(TAG, "Found WiFi credentials file at: %s", paths[i]);
            break;
        }
    }

    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    char line[128];
    int line_num = 0;

    // Read file line by line
    while (fgets(line, sizeof(line), f) != NULL && line_num < 3) {
        // Remove trailing newline and carriage return
        line[strcspn(line, "\r\n")] = 0;

        // Skip empty lines and comments
        if (strlen(line) == 0 || line[0] == '#') {
            continue;
        }

        switch (line_num) {
        case 0:  // SSID
            strncpy(ssid, line, WIFI_SSID_MAX_LEN - 1);
            ssid[WIFI_SSID_MAX_LEN - 1] = '\0';
            break;
        case 1:  // Password
            strncpy(password, line, WIFI_PASS_MAX_LEN - 1);
            password[WIFI_PASS_MAX_LEN - 1] = '\0';
            break;
        case 2:  // Device Name (Optional)
            config_manager_set_device_name(line);
            ESP_LOGI(TAG, "Loaded Device Name from SD: %s", line);
            break;
        }
        line_num++;
    }
    fclose(f);

    if (line_num >= 2) {
        ESP_LOGI(TAG, "Successfully read WiFi credentials from SD card");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Invalid wifi.txt format on SD card");
        return ESP_ERR_INVALID_ARG;
    }
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
