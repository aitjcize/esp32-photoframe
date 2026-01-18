#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "album_manager.h"
#include "axp_prot.h"
#include "color_palette.h"
#include "config.h"
#include "config_manager.h"
#include "display_manager.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_integration.h"
#include "http_server.h"
#include "i2c_bsp.h"
#include "image_processor.h"
#include "mdns_service.h"
#include "nvs_flash.h"
#include "ota_manager.h"
#include "power_manager.h"
#include "processing_settings.h"
#include "sdmmc_cmd.h"
#include "utils.h"
#include "wifi_manager.h"
#include "wifi_provisioning.h"

static const char *TAG = "main";

// Helper function to connect to WiFi with timeout
static bool connect_to_wifi_with_timeout(int timeout_seconds)
{
    char wifi_ssid[WIFI_SSID_MAX_LEN] = {0};
    char wifi_password[WIFI_PASS_MAX_LEN] = {0};

    ESP_ERROR_CHECK(wifi_manager_load_credentials(wifi_ssid, wifi_password));
    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", wifi_ssid);
    wifi_manager_connect(wifi_ssid, wifi_password);

    // Wait for WiFi connection (with timeout)
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    int retry_count = 0;
    while (!wifi_manager_is_connected() && retry_count < timeout_seconds) {
        if (retry_count % 10 == 0 && retry_count > 0) {
            ESP_LOGI(TAG, "WiFi connecting... (%d seconds elapsed)", retry_count);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry_count++;
    }

    if (wifi_manager_is_connected()) {
        ESP_LOGI(TAG, "WiFi connected after %d seconds", retry_count);
        return true;
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout after %d seconds", timeout_seconds);
        return false;
    }
}

static esp_err_t init_sdcard(void)
{
    ESP_LOGI(TAG, "Initializing SD card");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 16 * 1024 * 3};

    sdmmc_card_t *card;
    const char mount_point[] = SDCARD_MOUNT_POINT;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    // Configure SD card pins for ESP32-S3 PhotoPainter
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = GPIO_NUM_39;  // SDMMC_CLK
    slot_config.cmd = GPIO_NUM_41;  // SDMMC_CMD
    slot_config.d0 = GPIO_NUM_40;   // SDMMC_D0
    slot_config.d1 = GPIO_NUM_1;    // SDMMC_D1
    slot_config.d2 = GPIO_NUM_2;    // SDMMC_D2
    slot_config.d3 = GPIO_NUM_38;   // SDMMC_D3

    // Retry SD card initialization up to 5 times with delays
    esp_err_t ret = ESP_FAIL;
    const int retries = 3;
    for (int retry = 0; retry < retries; retry++) {
        if (retry > 0) {
            ESP_LOGW(TAG, "SD card init failed, retrying... (attempt %d/%d)", retry + 1, retries);
            vTaskDelay(pdMS_TO_TICKS(500));  // Wait 500ms before retry
        }

        ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

        if (ret == ESP_OK) {
            break;  // Success
        }
    }

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem after %d attempts", retries);
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card after %d attempts (%s)", retries,
                     esp_err_to_name(ret));
        }
        return ret;
    }

    sdmmc_card_print_info(stdout, card);

    // Poll sdcard status until it's ready
    ESP_LOGI(TAG, "Waiting for SD card to be ready...");
    while (sdmmc_get_status(card) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "SD card ready");

    struct stat st;
    if (stat(IMAGE_DIRECTORY, &st) != 0) {
        ESP_LOGI(TAG, "Creating image directory: %s", IMAGE_DIRECTORY);
        mkdir(IMAGE_DIRECTORY, 0775);
    }

    ESP_LOGI(TAG, "SD card initialized successfully");
    return ESP_OK;
}

static void button_task(void *arg)
{
    bool last_boot_state = gpio_get_level(BOOT_BUTTON_GPIO);
    bool last_key_state = gpio_get_level(KEY_BUTTON_GPIO);
    bool current_boot_state, current_key_state;
    uint32_t boot_press_time = 0;
    uint32_t key_press_time = 0;

    while (1) {
        current_boot_state = gpio_get_level(BOOT_BUTTON_GPIO);
        current_key_state = gpio_get_level(KEY_BUTTON_GPIO);

        // Handle BOOT button
        if (current_boot_state == 0 && last_boot_state == 1) {
            boot_press_time = xTaskGetTickCount();
        } else if (current_boot_state == 1 && last_boot_state == 0) {
            uint32_t duration = (xTaskGetTickCount() - boot_press_time) * portTICK_PERIOD_MS;

            if (duration > 50 && duration < 3000) {
                ESP_LOGI(TAG, "Boot button pressed, resetting sleep timer");
                power_manager_reset_sleep_timer();
            }
        }

        // Handle KEY button - trigger rotation
        if (current_key_state == 0 && last_key_state == 1) {
            key_press_time = xTaskGetTickCount();
        } else if (current_key_state == 1 && last_key_state == 0) {
            uint32_t duration = (xTaskGetTickCount() - key_press_time) * portTICK_PERIOD_MS;

            if (duration > 50 && duration < 3000) {
                ESP_LOGI(TAG, "Key button pressed, triggering rotation");
                power_manager_reset_sleep_timer();
                display_manager_rotate_from_sdcard();
            }
        }

        last_boot_state = current_boot_state;
        last_key_state = current_key_state;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    // Check reset reason to detect crashes
    esp_reset_reason_t reset_reason = esp_reset_reason();
    const char *reset_reason_str;
    switch (reset_reason) {
    case ESP_RST_POWERON:
        reset_reason_str = "Power-on reset";
        break;
    case ESP_RST_SW:
        reset_reason_str = "Software reset";
        break;
    case ESP_RST_PANIC:
        reset_reason_str = "Exception/panic";
        break;
    case ESP_RST_INT_WDT:
        reset_reason_str = "Interrupt watchdog";
        break;
    case ESP_RST_TASK_WDT:
        reset_reason_str = "Task watchdog";
        break;
    case ESP_RST_WDT:
        reset_reason_str = "Other watchdog";
        break;
    case ESP_RST_DEEPSLEEP:
        reset_reason_str = "Deep sleep wake";
        break;
    case ESP_RST_BROWNOUT:
        reset_reason_str = "Brownout reset";
        break;
    default:
        reset_reason_str = "Unknown";
        break;
    }
    ESP_LOGI(TAG, "PhotoFrame starting... (Reset reason: %s)", reset_reason_str);

    // Log initial memory state
    ESP_LOGI(TAG, "Free heap: %lu bytes, Largest free block: %lu bytes", esp_get_free_heap_size(),
             heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    // Initialize I2C bus (required for AXP2101 communication)
    ESP_LOGI(TAG, "Initializing I2C bus...");
    i2c_master_Init();

    // Initialize AXP2101 power management chip (required for e-paper display power)
    ESP_LOGI(TAG, "Initializing AXP2101 power management...");
    axp_i2c_prot_init();
    axp_cmd_init();
    ESP_LOGI(TAG, "AXP2101 initialized");

    // Wait for power rails to stabilize after AXP2101 initialization
    // The AXP2101 enables DC1, ALDO3, ALDO4 at 3.3V which power the SD card
    // Increase delay to ensure SD card power is fully stable
    ESP_LOGI(TAG, "Waiting for power rails to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(500));  // Increased from 200ms to 500ms

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(config_manager_init());

    ESP_ERROR_CHECK(image_processor_init());

    ESP_ERROR_CHECK(display_manager_init());

    ESP_ERROR_CHECK(processing_settings_init());

    ESP_ERROR_CHECK(color_palette_init());

    // Initialize power manager early to detect wakeup cause
    ESP_ERROR_CHECK(power_manager_init());

    ret = init_sdcard();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card initialization failed - triggering hard reset");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Give time for log to flush
        axp_shutdown();                   // Hard power-off
        // Won't reach here
    }

    // Initialize album manager
    ESP_ERROR_CHECK(album_manager_init());

    // Check wake-up source with priority: Timer > KEY > BOOT
    if (power_manager_is_timer_wakeup() || power_manager_is_key_button_wakeup()) {
        // Check rotation mode and HA configuration
        rotation_mode_t rotation_mode = config_manager_get_rotation_mode();
        bool ha_configured = ha_is_configured();
        bool wifi_connected = false;

        // Initialize WiFi if needed (URL mode always needs it, SD card mode only if HA configured)
        if (rotation_mode == ROTATION_MODE_URL || ha_configured) {
            ESP_LOGI(TAG, "Initializing WiFi for %s",
                     rotation_mode == ROTATION_MODE_URL ? "URL rotation" : "HA battery post");
            ESP_ERROR_CHECK(wifi_manager_init());

            if (connect_to_wifi_with_timeout(60)) {
                wifi_connected = true;
                ESP_LOGI(TAG, "WiFi connected");
            } else {
                ESP_LOGW(TAG, "WiFi connection timeout");
            }
        }

        // Trigger rotation
        trigger_image_rotation();

        // Post battery data to HA if WiFi is connected and HA is configured
        if (wifi_connected && ha_configured) {
            ESP_LOGI(TAG, "Posting battery status to Home Assistant");
            esp_err_t ha_err = ha_post_battery_info();
            if (ha_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to post battery to HA, continuing anyway");
            }
        }

        // Go directly back to sleep without starting HTTP server
        ESP_LOGI(TAG, "Auto-rotate complete, going back to sleep");
        power_manager_enter_sleep();
        // Won't reach here after sleep
    } else if (power_manager_is_boot_button_wakeup()) {
        ESP_LOGI(TAG, "BOOT button wakeup detected - starting WiFi and HTTP server");
        // Continue with normal initialization
    }

    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(wifi_provisioning_init());

    if (!wifi_provisioning_is_provisioned()) {
        // Try to load WiFi credentials from SD card first
        char sd_ssid[WIFI_SSID_MAX_LEN] = {0};
        char sd_password[WIFI_PASS_MAX_LEN] = {0};

        if (wifi_manager_load_credentials_from_sdcard(sd_ssid, sd_password) == ESP_OK) {
            ESP_LOGI(TAG, "===========================================");
            ESP_LOGI(TAG, "WiFi credentials found on SD card!");
            ESP_LOGI(TAG, "Saving to NVS and connecting...");
            ESP_LOGI(TAG, "===========================================");

            // Save credentials to NVS
            if (wifi_manager_save_credentials(sd_ssid, sd_password) == ESP_OK) {
                ESP_LOGI(TAG, "WiFi credentials saved to NVS");
                ESP_LOGI(TAG, "Restarting to connect with new credentials...");
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_restart();
            } else {
                ESP_LOGE(TAG, "Failed to save WiFi credentials to NVS");
            }
        }

        // No SD card credentials found, start captive portal provisioning
        ESP_LOGI(TAG, "===========================================");
        ESP_LOGI(TAG, "No WiFi credentials found - Starting AP mode");
        ESP_LOGI(TAG, "===========================================");
        ESP_LOGI(TAG, "Option 1: Place wifi.txt on SD card with:");
        ESP_LOGI(TAG, "  Line 1: WiFi SSID");
        ESP_LOGI(TAG, "  Line 2: WiFi Password");
        ESP_LOGI(TAG, "  Then restart the device");
        ESP_LOGI(TAG, "===========================================");
        ESP_LOGI(TAG, "Option 2: Use captive portal:");
        ESP_LOGI(TAG, "1. Connect to WiFi: PhotoFrame-Setup");
        ESP_LOGI(TAG, "2. Open browser to: http://192.168.4.1");
        ESP_LOGI(TAG, "3. Enter your WiFi credentials");
        ESP_LOGI(TAG, "===========================================");

        ESP_ERROR_CHECK(wifi_provisioning_start_ap());

        while (!wifi_provisioning_is_provisioned()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        ESP_LOGI(TAG, "WiFi credentials saved! Restarting...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    if (connect_to_wifi_with_timeout(30)) {
        // Start mDNS service
        ESP_ERROR_CHECK(mdns_service_init());
    } else {
        ESP_LOGW(TAG, "Failed to connect to WiFi - clearing credentials");
        nvs_handle_t nvs_handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
            nvs_erase_key(nvs_handle, NVS_WIFI_SSID_KEY);
            nvs_erase_key(nvs_handle, NVS_WIFI_PASS_KEY);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }
        ESP_LOGI(TAG, "Restarting to enter provisioning mode...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    ESP_ERROR_CHECK(http_server_init());
    ESP_ERROR_CHECK(ha_integration_init());

    // Initialize OTA manager (checks for updates on startup and periodically)
    ESP_ERROR_CHECK(ota_manager_init());

    if (wifi_manager_is_connected()) {
        char ip_str[16];
        wifi_manager_get_ip(ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "===========================================");
        ESP_LOGI(TAG, "Web interface available at: http://%s", ip_str);
        ESP_LOGI(TAG, "Or use: http://photoframe.local");
        ESP_LOGI(TAG, "===========================================");
    }

    xTaskCreate(button_task, "button_task", 8192, NULL, 5, NULL);

    // Mark system as ready for HTTP requests after all initialization is complete
    http_server_set_ready();

    ESP_LOGI(TAG, "PhotoFrame started successfully");
}
