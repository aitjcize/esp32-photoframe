#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "axp_prot.h"
#include "ble_wake_service.h"
#include "config.h"
#include "display_manager.h"
#include "dns_server.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"
#include "i2c_bsp.h"
#include "image_processor.h"
#include "mdns_service.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "power_manager.h"
#include "sdcard_bsp.h"
#include "sdmmc_cmd.h"
#include "wifi_manager.h"
#include "wifi_provisioning.h"

static const char *TAG = "main";

static esp_err_t init_sdcard(void)
{
    ESP_LOGI(TAG, "Initializing SD card");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 16 * 1024};

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
                display_manager_handle_timer_wakeup();
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

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize SD card (optional - device can still work without it for WiFi setup)
    ret = init_sdcard();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card initialization failed - triggering hard reset");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Give time for log to flush
        axp_shutdown();                   // Hard power-off
        // Won't reach here
    }

    ESP_ERROR_CHECK(image_processor_init());

    ESP_ERROR_CHECK(display_manager_init());

    // Initialize power manager early to detect wakeup cause
    ESP_ERROR_CHECK(power_manager_init());

    // Check wake-up source with priority: Timer > KEY > BOOT
    if (power_manager_is_timer_wakeup()) {
        ESP_LOGI(TAG, "Timer wakeup detected - auto-rotate and sleep");
        display_manager_handle_timer_wakeup();

        // Go directly back to sleep without starting WiFi or HTTP server
        ESP_LOGI(TAG, "Auto-rotate complete, going back to sleep");
        power_manager_enter_sleep_with_timer(display_manager_get_rotate_interval());
        // Won't reach here after sleep
    } else if (power_manager_is_key_button_wakeup()) {
        ESP_LOGI(TAG, "KEY button wakeup detected - rotate and sleep");
        display_manager_handle_timer_wakeup();

        // Go directly back to sleep without starting WiFi or HTTP server
        // Need to reschedule auto-rotate timer if enabled (RTC timer is one-shot)
        ESP_LOGI(TAG, "Manual rotation complete, going back to sleep");
        power_manager_trigger_sleep();
        // Won't reach here after sleep
    } else if (power_manager_is_boot_button_wakeup()) {
        ESP_LOGI(TAG, "BOOT button wakeup detected - starting WiFi and HTTP server");
        // Continue with normal initialization
    }

    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(wifi_provisioning_init());

    if (!wifi_provisioning_is_provisioned()) {
        ESP_LOGI(TAG, "===========================================");
        ESP_LOGI(TAG, "No WiFi credentials found - Starting AP mode");
        ESP_LOGI(TAG, "===========================================");
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

    char wifi_ssid[WIFI_SSID_MAX_LEN] = {0};
    char wifi_password[WIFI_PASS_MAX_LEN] = {0};

    ESP_ERROR_CHECK(wifi_manager_load_credentials(wifi_ssid, wifi_password));
    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", wifi_ssid);

    if (wifi_manager_connect(wifi_ssid, wifi_password) == ESP_OK) {
        char ip_str[16];
        wifi_manager_get_ip(ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "Connected to WiFi, IP: %s", ip_str);

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

    // Initialize BLE wake service (will only start if enabled in NVS)
    ESP_ERROR_CHECK(ble_wake_service_init());
    if (ble_wake_service_get_enabled()) {
        ESP_LOGI(TAG, "BLE wake mode enabled, starting BLE advertising");
        ESP_ERROR_CHECK(ble_wake_service_start());
        
        // Enable automatic light sleep only when on battery power
        // When USB is connected, power consumption is not a concern
        // Set CONFIG_PM_DEBUG_ENABLE_USB=y in sdkconfig to test with USB connected
        bool enable_pm = !axp_is_usb_connected();
        
#if 1
        enable_pm = true;  // Force enable for debugging even with USB
        ESP_LOGI(TAG, "PM_DEBUG: Forcing automatic light sleep even with USB connected");
#endif
        
        if (enable_pm) {
            esp_pm_config_t pm_config = {
                .max_freq_mhz = 160,
                .min_freq_mhz = 80,
                .light_sleep_enable = true
            };
            esp_err_t pm_err = esp_pm_configure(&pm_config);
            if (pm_err == ESP_OK) {
                ESP_LOGI(TAG, "Automatic light sleep enabled (BLE remains active)");
            } else {
                ESP_LOGW(TAG, "Failed to enable automatic light sleep: %s", esp_err_to_name(pm_err));
            }
        } else {
            ESP_LOGI(TAG, "USB connected - automatic light sleep disabled");
        }
    }

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
