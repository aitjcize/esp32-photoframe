#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "board_hal.h"
#include "color_palette.h"
#include "config.h"
#include "config_manager.h"
#include "display_manager.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_integration.h"
#include "http_server.h"
#include "image_processor.h"
#include "mdns_service.h"
#include "memfs.h"
#include "nvs_flash.h"
#include "ota_manager.h"
#include "periodic_tasks.h"
#include "power_manager.h"
#include "processing_settings.h"
#include "utils.h"
#include "wifi_manager.h"
#include "wifi_provisioning.h"

#ifdef CONFIG_HAS_SDCARD
#include "album_manager.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#endif

static const char *TAG = "main";

// Periodic callback for SNTP sync
static esp_err_t sntp_sync_periodic_callback(void)
{
    ESP_LOGI(TAG, "Periodic SNTP sync triggered");

    // Force SNTP to sync again (timezone is already set by config_manager)
    esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();

    // Wait briefly for sync (non-blocking approach)
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 5;  // Shorter timeout for periodic sync

    while (timeinfo.tm_year < (2025 - 1900) && ++retry < retry_count) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (timeinfo.tm_year >= (2025 - 1900)) {
        char strftime_buf[64];
        strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
        ESP_LOGI(TAG, "SNTP sync successful: %s", strftime_buf);

        // Update external RTC with synced time
        time(&now);
        if (board_hal_rtc_is_available()) {
            esp_err_t ret = board_hal_rtc_set_time(now);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Updated external RTC with SNTP time");
            } else {
                ESP_LOGW(TAG, "Failed to update external RTC: %s", esp_err_to_name(ret));
            }
        }

        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "SNTP sync timeout, will retry next period");
        return ESP_ERR_TIMEOUT;
    }
}

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

#ifdef CONFIG_HAS_SDCARD
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
#endif

static void button_task(void *arg)
{
    bool last_boot_state = 1;  // Default distinct from current to avoid triggers if NC
    if (BOARD_HAL_WAKEUP_KEY != GPIO_NUM_NC) {
        last_boot_state = gpio_get_level(BOARD_HAL_WAKEUP_KEY);
    }

    bool last_key_state = 1;
    if (BOARD_HAL_ROTATE_KEY != GPIO_NUM_NC) {
        last_key_state = gpio_get_level(BOARD_HAL_ROTATE_KEY);
    }

    bool current_boot_state, current_key_state;
    uint32_t boot_press_time = 0;
    uint32_t key_press_time = 0;

    while (1) {
        if (BOARD_HAL_WAKEUP_KEY != GPIO_NUM_NC) {
            current_boot_state = gpio_get_level(BOARD_HAL_WAKEUP_KEY);

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
            last_boot_state = current_boot_state;
        }

        if (BOARD_HAL_ROTATE_KEY != GPIO_NUM_NC) {
            current_key_state = gpio_get_level(BOARD_HAL_ROTATE_KEY);

            // Handle KEY button - trigger rotation
            if (current_key_state == 0 && last_key_state == 1) {
                key_press_time = xTaskGetTickCount();
            } else if (current_key_state == 1 && last_key_state == 0) {
                uint32_t duration = (xTaskGetTickCount() - key_press_time) * portTICK_PERIOD_MS;

                if (duration > 50 && duration < 3000) {
                    ESP_LOGI(TAG, "Key button pressed, triggering rotation");
                    power_manager_reset_sleep_timer();
                    trigger_image_rotation();
                    ha_notify_update();
                }
            }
            last_key_state = current_key_state;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void deep_sleep_wake_main(void)
{
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

    // Start HTTP server for 10 seconds to allow config modifications
    if (wifi_connected && ha_configured) {
        power_manager_reset_sleep_timer();

        // Check and run periodic tasks (OTA check, SNTP sync if due)
        ESP_LOGI(TAG, "Checking periodic tasks...");
        periodic_tasks_check_and_run();

        ESP_LOGI(TAG, "Starting HTTP server for 10 seconds before sleep");
        power_manager_reset_sleep_timer();

        // Start mDNS service so HA can resolve photoframe.local
        ESP_ERROR_CHECK(mdns_service_init());

        ESP_ERROR_CHECK(http_server_init());
        http_server_set_ready();

        ha_notify_online();
    }

    // Trigger rotation
    power_manager_reset_sleep_timer();
    trigger_image_rotation();

    // Notify HA that data has been updated (after both OTA check and rotation)
    if (wifi_connected && ha_configured) {
        ha_notify_update();

        // Keep server running for 10 seconds
        ESP_LOGI(TAG, "HTTP server available for config changes");
        vTaskDelay(pdMS_TO_TICKS(10000));

        ESP_LOGI(TAG, "HTTP server window closed");
    }

    // Go back to sleep (offline notification sent inside power_manager_enter_sleep)
    ESP_LOGI(TAG, "Auto-rotate complete, going back to sleep");
    power_manager_enter_sleep();
    // Won't reach here after sleep
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
    ESP_LOGI(TAG, "Board type: %s", BOARD_HAL_NAME);

    // Log initial memory state
    ESP_LOGI(TAG, "Free heap: %lu bytes, Largest free block: %lu bytes", esp_get_free_heap_size(),
             heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    // Initialize Power HAL (wraps PMIC/Charger specific logic and Sensors)
    ESP_LOGI(TAG, "Initializing Power HAL...");
    ESP_ERROR_CHECK(board_hal_init());
    ESP_LOGI(TAG, "Power HAL initialized");

#ifndef CONFIG_HAS_SDCARD
    // Initialize RAM filesystem for temporary images
    ESP_LOGI(TAG, "Mounting RAM filesystem...");
    ESP_ERROR_CHECK(memfs_mount(TEMP_MOUNT_POINT, 10));
#endif

    // Initialize external RTC (via HAL)
    ESP_LOGI(TAG, "Initializing RTC...");
    esp_err_t rtc_ret = board_hal_rtc_init();
    if (rtc_ret == ESP_OK) {
        ESP_LOGI(TAG, "RTC initialized successfully");
    } else if (rtc_ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "External RTC not supported on this board");
    } else {
        ESP_LOGW(TAG, "RTC initialization failed: %s", esp_err_to_name(rtc_ret));
    }

    // Wait for power rails to stabilize after AXP2101 initialization
    // The AXP2101 enables DC1, ALDO3, ALDO4 at 3.3V which power the SD card (if present)
    // Increase delay to ensure power is fully stable
    ESP_LOGI(TAG, "Waiting for power rails to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(500));  // Increased from 200ms to 500ms

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(config_manager_init());

    // Always restore time from external RTC (internal RTC is inaccurate)
    ESP_LOGI(TAG, "Checking external RTC for time restoration...");
    bool time_restored = false;

    if (board_hal_rtc_is_available()) {
        time_t external_time;
        esp_err_t ret = board_hal_rtc_get_time(&external_time);
        if (ret == ESP_OK) {
            struct tm external_timeinfo;
            localtime_r(&external_time, &external_timeinfo);

            if (external_timeinfo.tm_year >= (2025 - 1900)) {
                // External RTC has valid time, restore it
                struct timeval tv = {.tv_sec = external_time, .tv_usec = 0};
                settimeofday(&tv, NULL);
                ESP_LOGI(TAG, "Restored time from external RTC: %04d-%02d-%02d %02d:%02d:%02d",
                         external_timeinfo.tm_year + 1900, external_timeinfo.tm_mon + 1,
                         external_timeinfo.tm_mday, external_timeinfo.tm_hour,
                         external_timeinfo.tm_min, external_timeinfo.tm_sec);
                time_restored = true;
            } else {
                ESP_LOGW(TAG, "External RTC time invalid (year %d)",
                         external_timeinfo.tm_year + 1900);
            }
        } else {
            ESP_LOGW(TAG, "Failed to read external RTC: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGI(TAG, "External RTC not available, skipping time restoration");
    }

    // If external RTC failed or invalid, force SNTP sync (don't trust internal RTC)
    if (!time_restored) {
        ESP_LOGW(TAG,
                 "External RTC unavailable/invalid, will force SNTP sync after WiFi connection");
        // Force SNTP sync to run on next periodic_tasks_check_and_run()
        periodic_tasks_force_run(SNTP_TASK_NAME);
    }

    // Initialize periodic tasks system
    ESP_LOGI(TAG, "Initializing periodic tasks...");
    ESP_ERROR_CHECK(periodic_tasks_init());

    // Register SNTP sync as a daily task
    ESP_ERROR_CHECK(
        periodic_tasks_register(SNTP_TASK_NAME, sntp_sync_periodic_callback, 24 * 60 * 60));
    ESP_LOGI(TAG, "Registered SNTP sync as daily task");

    ESP_ERROR_CHECK(image_processor_init());

    ESP_ERROR_CHECK(display_manager_init());

    ESP_ERROR_CHECK(processing_settings_init());

    ESP_ERROR_CHECK(color_palette_init());

    ESP_ERROR_CHECK(power_manager_init());

    ESP_ERROR_CHECK(ota_manager_init());

#ifdef CONFIG_HAS_SDCARD
    ret = init_sdcard();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card initialization failed - triggering hard reset");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Give time for log to flush
        board_hal_shutdown();             // Hard power-off
        // Won't reach here
    }

    ESP_ERROR_CHECK(album_manager_init());
#else
    ESP_LOGI(TAG, "SD Card support disabled (No-SDCard Mode)");
#endif

    // Check wake-up source with priority: Timer > KEY > BOOT
    bool is_timer = power_manager_is_timer_wakeup();
    bool is_key = power_manager_is_key_button_wakeup();
    bool is_boot = power_manager_is_boot_button_wakeup();

    ESP_LOGI(TAG, "Wake-up detection: timer=%d, key=%d, boot=%d", is_timer, is_key, is_boot);

    if (is_timer || is_key) {
        ESP_LOGI(TAG, "Entering deep sleep wake path (timer or key button)");
        deep_sleep_wake_main();
        // Won't reach here after sleep
    } else if (is_boot) {
        ESP_LOGI(TAG, "BOOT button wakeup detected - starting WiFi and HTTP server");
        // Continue with normal initialization
    }

    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(wifi_provisioning_init());

    if (!wifi_provisioning_is_provisioned()) {
        bool creds_loaded = false;
#ifdef CONFIG_HAS_SDCARD
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
                creds_loaded = true;
            } else {
                ESP_LOGE(TAG, "Failed to save WiFi credentials to NVS");
            }
        }
#endif

        // No SD card credentials found, start captive portal provisioning
        if (!creds_loaded) {
            ESP_LOGI(TAG, "===========================================");
            ESP_LOGI(TAG, "No WiFi credentials found - Starting AP mode");
            ESP_LOGI(TAG, "===========================================");
#ifdef CONFIG_HAS_SDCARD
            ESP_LOGI(TAG, "Option 1: Place wifi.txt on SD card with:");
            ESP_LOGI(TAG, "  Line 1: WiFi SSID");
            ESP_LOGI(TAG, "  Line 2: WiFi Password");
            ESP_LOGI(TAG, "  Line 3: Device Name (optional, default: PhotoFrame)");
            ESP_LOGI(TAG, "  Then restart the device");
            ESP_LOGI(TAG, "===========================================");
#endif
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
    }

    if (connect_to_wifi_with_timeout(30)) {
        // Check and run periodic tasks (OTA check, SNTP sync if due)
        // Note: If RTC was invalid at boot, sntp_sync was already forced via
        // periodic_tasks_force_run()
        ESP_LOGI(TAG, "Checking periodic tasks...");
        periodic_tasks_check_and_run();

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

    xTaskCreate(button_task, "button_task", 8192, NULL, 5, NULL);

    // Perform the initial check on boot
    ota_check_for_update(NULL, 0);

    ESP_ERROR_CHECK(http_server_init());
    http_server_set_ready();

    if (wifi_manager_is_connected()) {
        char ip_str[16];
        wifi_manager_get_ip(ip_str, sizeof(ip_str));

        // Get sanitized hostname for mDNS
        const char *device_name = config_manager_get_device_name();
        char hostname[64];
        sanitize_hostname(device_name, hostname, sizeof(hostname));

        ESP_LOGI(TAG, "===========================================");
        ESP_LOGI(TAG, "Web interface available at: http://%s", ip_str);
        ESP_LOGI(TAG, "Or use: http://%s.local", hostname);
        ESP_LOGI(TAG, "===========================================");
    }

    // Notify HA that device is online (HA will poll for all data via REST API)
    ESP_LOGI(TAG, "Sending online notification to Home Assistant");
    ha_notify_online();

    ESP_LOGI(TAG, "PhotoFrame started successfully");
}
