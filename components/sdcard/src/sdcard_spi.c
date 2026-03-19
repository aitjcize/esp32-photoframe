#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdcard.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard_spi";

sdmmc_card_t *card_host = NULL;
static gpio_num_t s_power_en_pin = -1;

esp_err_t sdcard_init(const sdcard_config_t *config)
{
    if (!config) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }

    s_power_en_pin = config->power_en_pin;

    // Power up SD card if a power enable pin is configured
    if (s_power_en_pin >= 0) {
        gpio_config_t pwr_cfg = {
            .pin_bit_mask = (1ULL << s_power_en_pin),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&pwr_cfg);
        gpio_set_level(s_power_en_pin, 1);
        ESP_LOGI(TAG, "SD card power enabled (GPIO%d)", s_power_en_pin);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024 * 3,
    };

    // Configure SD card device on SPI
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = config->cs_pin;
    slot_config.host_id = config->host_id;

    ESP_LOGI(TAG, "Mounting SD card via SPI (Host=%d, CS=%d)", config->host_id, config->cs_pin);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    esp_err_t ret = esp_vfs_fat_sdspi_mount(config->mount_point, &host, &slot_config, &mount_config,
                                            &card_host);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem");
        } else if (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_NOT_FOUND || ret == 0x107) {
            ESP_LOGW(
                TAG,
                "SD card not detected or initialization failed (%s). Continuing in No-SDCard mode.",
                esp_err_to_name(ret));
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    if (card_host != NULL) {
        sdmmc_card_print_info(stdout, card_host);
        ESP_LOGI(TAG, "SD card mounted successfully");
        return ESP_OK;
    }

    return ESP_FAIL;
}

bool sdcard_is_mounted(void)
{
    return card_host != NULL;
}

void sdcard_deinit(void)
{
    if (card_host) {
        esp_vfs_fat_sdcard_unmount("/sdcard", card_host);
        card_host = NULL;
    }
    // Cut power to SD card to save power
    if (s_power_en_pin >= 0) {
        gpio_set_level(s_power_en_pin, 0);
        ESP_LOGI(TAG, "SD card power disabled (GPIO%d)", s_power_en_pin);
    }
}
