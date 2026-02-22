#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "sdcard.h"

static const char *TAG = "sdcard_internal";

#define SDlist "/sdcard"

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static bool s_mounted = false;

esp_err_t sdcard_init(const sdcard_config_t *config)
{
    ESP_LOGI(TAG, "Mounting Internal Flash FAT filesystem as virtual SD card...");

    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 5,
        .format_if_mount_failed = true, // Formatiert die 8.8MB beim ersten Start automatisch!
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE
    };

    // "storage" muss exakt dem Namen in der partitions.csv entsprechen
    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(SDlist, "storage", &mount_config, &s_wl_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount internal filesystem (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Internal FAT filesystem mounted successfully to %s", SDlist);
    s_mounted = true;

    return ESP_OK;
}

bool sdcard_is_mounted(void)
{
    return s_mounted;
}