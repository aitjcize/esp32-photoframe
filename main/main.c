#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "album_manager.h"
#include "board_hal.h"
#include "color_palette.h"
#include "config.h"
#include "config_manager.h"
#include "debug_log.h"
#include "display_manager.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_vfs_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// External RTC support
#ifdef CONFIG_EXT_RTC_ENABLED
#include "ext_rtc.h"
#endif

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
#include "splash_screen.h"
#include "storage.h"
#include "utils.h"
#include "wifi_manager.h"
#include "wifi_provisioning.h"

static const char *TAG = "main";

// Log the current wall-clock (defined below; forward-declared so the SNTP
// callback above the definition can use it too).
static void log_wall_clock(const char *label);

// Set (from the lwIP task) when SNTP actually applies a fresh time. The
// periodic sync below waits on this rather than on "is the clock already set",
// because on a timer wake the retained RTC clock is already a valid (but
// drifted) time — so it would otherwise return before the correction lands.
static volatile bool s_sntp_time_applied = false;

static void sntp_time_sync_notification(struct timeval *tv)
{
    s_sntp_time_applied = true;
}

// Periodic callback for SNTP sync
static esp_err_t sntp_sync_periodic_callback(void)
{
    ESP_LOGI(TAG, "Periodic SNTP sync triggered");

    s_sntp_time_applied = false;

    // Force SNTP to sync again (timezone is already set by config_manager)
    esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, config_manager_get_ntp_server());
    esp_sntp_setservername(1, "time.google.com");
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    sntp_set_time_sync_notification_cb(sntp_time_sync_notification);
    esp_sntp_init();

    // Wait for the server response to actually be APPLIED (settimeofday from the
    // lwIP task), not merely for the clock to read a valid year. Downstream, the
    // early-wake re-check re-reads the clock and depends on the corrected time
    // being in place before it decides whether the timer fired early.
    const int retry_count = 10;  // up to ~10 seconds
    for (int i = 0; i < retry_count && !s_sntp_time_applied; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!s_sntp_time_applied) {
        ESP_LOGW(TAG, "SNTP sync timeout, will retry next period");
        return ESP_ERR_TIMEOUT;
    }

    time_t now = 0;
    time(&now);
    log_wall_clock("SNTP sync");

    // Update external RTC with synced time
    if (board_hal_rtc_is_available()) {
        esp_err_t ret = board_hal_rtc_set_time(now);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Updated external RTC with SNTP time");
        } else {
            ESP_LOGW(TAG, "Failed to update external RTC: %s", esp_err_to_name(ret));
        }
    }

    // Reset rotate timer now that the clock is corrected
    power_manager_reset_rotate_timer();

    return ESP_OK;
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

        if (BOARD_HAL_CLEAR_KEY != GPIO_NUM_NC) {
            bool current_clear_state = gpio_get_level(BOARD_HAL_CLEAR_KEY);
            // Handle CLEAR button (active low assumed, similar to others?)
            // Assuming standard button behavior: press = 0, release = 1
            // But verify if it's the same. Usually buttons are pulled up.

            // Static state for clear button
            static bool last_clear_state = 1;
            static uint32_t clear_press_time = 0;

            if (current_clear_state == 0 && last_clear_state == 1) {
                clear_press_time = xTaskGetTickCount();
            } else if (current_clear_state == 1 && last_clear_state == 0) {
                uint32_t duration = (xTaskGetTickCount() - clear_press_time) * portTICK_PERIOD_MS;

                if (duration > 50 && duration < 3000) {
                    ESP_LOGI(TAG, "Clear button pressed, clearing display");
                    power_manager_reset_sleep_timer();
                    display_manager_clear();
                    ha_notify_update();
                }
            }
            last_clear_state = current_clear_state;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Bring up mDNS + the HTTP config server exactly once per wake. Idempotent so
// both the pre-rotation HA path and the post-rotation config-sync window can
// call it freely without tracking who started it. (Deep sleep reboots the app,
// so the guard resets each wake.)
static void ensure_http_server_running(void)
{
    static bool started = false;
    if (started) {
        return;
    }
    // Start mDNS so HA / the LAN can resolve photoframe.local.
    ESP_ERROR_CHECK(mdns_service_init());
    ESP_ERROR_CHECK(http_server_init());
    http_server_set_ready();
    started = true;
    ESP_LOGI(TAG, "HTTP server started");
}

// Log the current wall-clock in a uniform, greppable format:
//   Wall-clock (<label>): <YYYY-MM-DD HH:MM:SS> (epoch <n>)
// The ESP_LOG "I (ms)" prefixes reset every boot, so each wake needs a real-time
// anchor to correlate against the server / HA logs. Logged at boot (from the
// external RTC, or the internal clock when there's no RTC) and again from the
// SNTP callback — the "SNTP sync" line is the trustworthy time on RTC-less boards.
static void log_wall_clock(const char *label)
{
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Wall-clock (%s): %s (epoch %ld)", label, buf, (long) now);
}

void deep_sleep_wake_main(wakeup_source_t wakeup_src)
{
    bool is_button_wake = (wakeup_src == WAKEUP_SOURCE_ROTATE_BUTTON);
    // Check rotation mode and HA configuration
    rotation_mode_t rotation_mode = config_manager_get_rotation_mode();
    bool ha_configured = ha_is_configured();
    bool wifi_connected = false;

    // Early-wake check before spending power on WiFi: on boards with an
    // external RTC the corrected time is already restored at this point, so
    // a wake that fired early due to RTC drift can go back to sleep for the
    // remainder without a WiFi connection. Boards without an external RTC
    // still believe they're on time here; for them the check repeats after
    // NTP sync below.
    // Exception: ROTATE button press always rotates immediately.
    int early_seconds = power_manager_get_seconds_until_wake_target();
    if (!is_button_wake && early_seconds > EARLY_WAKE_TOLERANCE_SEC) {
        ESP_LOGI(TAG, "Woke %d seconds before scheduled rotation, going back to sleep",
                 early_seconds);
        power_manager_enter_sleep();
        // Won't reach here after sleep
    }

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

    if (wifi_connected) {
        power_manager_reset_sleep_timer();

        // Check and run periodic tasks (OTA check, SNTP sync if due). When SNTP
        // actually corrects the clock it logs a "Wall-clock (SNTP sync)" line —
        // the trustworthy anchor on RTC-less boards.
        ESP_LOGI(TAG, "Checking periodic tasks...");
        periodic_tasks_check_and_run();
    }

    // Re-check now that the clock is as corrected as it will get (NTP sync
    // above may have pulled it backward on boards without an external RTC).
    early_seconds = power_manager_get_seconds_until_wake_target();
    if (!is_button_wake && early_seconds > EARLY_WAKE_TOLERANCE_SEC) {
        ESP_LOGI(TAG, "Woke %d seconds before scheduled rotation, going back to sleep",
                 early_seconds);
        power_manager_enter_sleep();
        // Won't reach here after sleep
    }

    // Whether this wake should actually rotate. Home Assistant can veto a
    // scheduled rotation (e.g. nobody home / night) via the notify response.
    bool should_rotate = true;

    // Bring the config server up before rotating so HA can reach us and the
    // notify response can carry the rotation decision.
    if (wifi_connected && ha_configured) {
        power_manager_reset_sleep_timer();
        ensure_http_server_running();

        // Piggyback the rotation decision on the online notification. The gate
        // is moot for a ROTATE button press (that always rotates), so don't ask
        // for or act on the decision there. Strictly fail-closed otherwise: WiFi
        // is up but we couldn't ask HA, so don't rotate. (A total WiFi failure
        // never reaches here, so it rotates as normal.)
        esp_err_t notify_err = ha_notify_online(is_button_wake ? NULL : &should_rotate);
        if (!is_button_wake && notify_err != ESP_OK) {
            ESP_LOGW(TAG, "Could not reach Home Assistant to check rotation; skipping");
            utils_set_last_fetch_error("Could not reach Home Assistant to check rotation");
            should_rotate = false;
        }
    }

    // Honor an HA veto (timer wakes only — a ROTATE button press always rotates).
    if (!is_button_wake && !should_rotate) {
        ESP_LOGI(TAG, "Rotation skipped by Home Assistant, going back to sleep");
        power_manager_enter_sleep();
        // Won't reach here after sleep
    }

    // Trigger rotation
    power_manager_reset_sleep_timer();
    trigger_image_rotation();

    // Notify HA that data has been updated (after both OTA check and rotation)
    if (wifi_connected && ha_configured) {
        ha_notify_update();
    }

    // Keep the HTTP server up briefly so a late config change — or a server-side
    // config pull — can reach us. HA frames always get a short window; a server
    // that asked us to wait (X-Post-Rotate-Wait-Sec, meaning it wants to pull our
    // config) extends it, and gets a window even without HA — URL-mode frames
    // don't start the server before rotating, so we start it on demand here.
    int hold_sec = (wifi_connected && ha_configured) ? HA_CONFIG_WINDOW_SEC : 0;
    int server_wait = utils_get_post_rotate_wait_sec();
    if (server_wait > hold_sec) {
        hold_sec = server_wait;
    }
    if (wifi_connected && hold_sec > 0) {
        ensure_http_server_running();  // no-op if the HA path already started it
        ESP_LOGI(TAG, "HTTP server available for config sync (%d s)", hold_sec);
        vTaskDelay(pdMS_TO_TICKS(hold_sec * 1000));
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

    // Initialize Board HAL
    ESP_LOGI(TAG, "Initializing Board HAL...");
    ESP_ERROR_CHECK(board_hal_init());
    ESP_LOGI(TAG, "Board HAL initialized");

    // Initialize the storage subsystem (handles SD, LittleFS, MemFS fallbacks)
    ESP_LOGI(TAG, "Initializing storage subsystem...");
    ESP_ERROR_CHECK(storage_init());

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

    // Start mirroring console logs to storage if debug logging is enabled.
    debug_log_init();

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

    // One boot-time anchor for log correlation, whatever the source. On
    // external-RTC boards this is the true wake time; on RTC-less boards it's the
    // internal clock's (untrusted) estimate — corrected by the "SNTP sync" line
    // once NTP runs.
    log_wall_clock(time_restored ? "external RTC" : "internal, pre-sync");

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

    ESP_ERROR_CHECK(album_manager_init());

    // Check wake-up source
    wakeup_source_t wakeup_src = power_manager_get_wakeup_source();
    ESP_LOGI(TAG, "Wake-up source: %d", wakeup_src);

    switch (wakeup_src) {
    case WAKEUP_SOURCE_CLEAR_BUTTON:
        ESP_LOGI(TAG, "CLEAR button wakeup detected - clearing display and sleeping");
        board_hal_init();             // Ensure HAL is active
        display_manager_init();       // Initialize display
        display_manager_clear();      // Clear screen
        power_manager_enter_sleep();  // Go back to sleep
        // Won't reach here
        break;

    case WAKEUP_SOURCE_TIMER:
    case WAKEUP_SOURCE_ROTATE_BUTTON:
        ESP_LOGI(TAG, "Entering deep sleep wake path (timer or rotate button)");
        deep_sleep_wake_main(wakeup_src);
        // Won't reach here after sleep
        break;

    case WAKEUP_SOURCE_BOOT_BUTTON:
        ESP_LOGI(TAG, "BOOT button wakeup detected - starting WiFi and HTTP server");
        // Continue with normal initialization
        break;

    default:
        // Cold boot or other wakeup - continue with normal initialization
        break;
    }

    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(wifi_provisioning_init());

    if (!wifi_provisioning_is_provisioned()) {
        bool creds_loaded = false;

        // Try to load WiFi credentials from storage
        char sd_ssid[WIFI_SSID_MAX_LEN] = {0};
        char sd_password[WIFI_PASS_MAX_LEN] = {0};

        if (storage_read_wifi_credentials(sd_ssid, sd_password) == ESP_OK) {
            ESP_LOGI(TAG, "===========================================");
            ESP_LOGI(TAG, "WiFi credentials found on storage!");
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

        // No credentials found, start captive portal provisioning
        if (!creds_loaded) {
            ESP_LOGI(TAG, "===========================================");
            ESP_LOGI(TAG, "No WiFi credentials found - Starting AP mode");
            ESP_LOGI(TAG, "===========================================");

            // Out-of-box setup: give the user more time to scan the QR code
            // and provision via the app before the device auto-sleeps.
            power_manager_set_auto_sleep_timeout(OOBE_AUTO_SLEEP_TIMEOUT_SEC);

            // Show OOBE splash screen with WiFi QR code
            splash_screen_display();

            if (storage_has_persistent_storage()) {
                ESP_LOGI(TAG, "Option 1: Place wifi.txt on root of storage with:");
                ESP_LOGI(TAG, "  Line 1: WiFi SSID");
                ESP_LOGI(TAG, "  Line 2: WiFi Password");
                ESP_LOGI(TAG, "  Line 3: Device Name (optional, default: PhotoFrame)");
                ESP_LOGI(TAG, "  Then restart the device");
                ESP_LOGI(TAG, "===========================================");
            }
            ESP_LOGI(TAG, "Option 2: Use captive portal:");
            ESP_LOGI(TAG, "1. Connect to WiFi: PhotoFrame - XXXXXX");
            ESP_LOGI(TAG, "2. Open browser to: http://192.168.4.1");
            ESP_LOGI(TAG, "3. Enter your WiFi credentials");
            ESP_LOGI(TAG, "===========================================");

            ESP_ERROR_CHECK(wifi_provisioning_start_ap());

            while (!wifi_provisioning_is_provisioned()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            // Set flag to show setup-complete screen after restart
            nvs_handle_t nvs_handle;
            if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
                nvs_set_u8(nvs_handle, NVS_SETUP_COMPLETE_KEY, 1);
                nvs_commit(nvs_handle);
                nvs_close(nvs_handle);
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

        // Show setup-complete screen if just provisioned
        nvs_handle_t nvs_handle;
        uint8_t setup_complete = 0;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
            if (nvs_get_u8(nvs_handle, NVS_SETUP_COMPLETE_KEY, &setup_complete) == ESP_OK &&
                setup_complete == 1) {
                nvs_erase_key(nvs_handle, NVS_SETUP_COMPLETE_KEY);
                nvs_commit(nvs_handle);
                ESP_LOGI(TAG, "First boot after provisioning — showing setup complete screen");
                splash_screen_display_setup_complete(hostname);
            }
            nvs_close(nvs_handle);
        }
    }

    // Notify HA that device is online (HA will poll for all data via REST API).
    // This is the always-on / cold-boot path, so the rotation-gate response
    // isn't used here.
    ESP_LOGI(TAG, "Sending online notification to Home Assistant");
    ha_notify_online(NULL);

    ESP_LOGI(TAG, "PhotoFrame started successfully");

    // Delay OTA check to avoid competing with boot-time network activity
    vTaskDelay(pdMS_TO_TICKS(10000));
    ota_check_for_update(NULL, 0);
}
