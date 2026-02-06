#ifndef CONFIG_H
#define CONFIG_H

#include <driver/gpio.h>

// Uncomment to debug deep sleep wake
// #define DEBUG_DEEP_SLEEP_WAKE

typedef enum {
    ROTATION_MODE_SDCARD = 0,
    ROTATION_MODE_URL = 1,
    ROTATION_MODE_AI = 2
} rotation_mode_t;

typedef enum { SD_ROTATION_RANDOM = 0, SD_ROTATION_SEQUENTIAL = 1 } sd_rotation_mode_t;

typedef enum {
    DISPLAY_ORIENTATION_LANDSCAPE = 0,
    DISPLAY_ORIENTATION_PORTRAIT = 1
} display_orientation_t;

#define LED_RED_GPIO GPIO_NUM_45
#define LED_GREEN_GPIO GPIO_NUM_42

#define DEVICE_NAME_MAX_LEN 64
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64
#define IMAGE_URL_MAX_LEN 256
#define HA_URL_MAX_LEN 256
#define ROTATION_MODE_MAX_LEN 16
#define TIMEZONE_MAX_LEN 64
#define ACCESS_TOKEN_MAX_LEN 512
#define HTTP_HEADER_KEY_MAX_LEN 64
#define HTTP_HEADER_VALUE_MAX_LEN 512

#define DEFAULT_DEVICE_NAME "PhotoFrame"
#define DEFAULT_WIFI_SSID "PhotoFrame"
#define DEFAULT_WIFI_PASSWORD "photoframe123"
#define DEFAULT_IMAGE_URL "https://loremflickr.com/800/480"
#define DEFAULT_HA_URL ""
#define DEFAULT_TIMEZONE "UTC0"

#define SDCARD_MOUNT_POINT "/sdcard"
#define IMAGE_DIRECTORY SDCARD_MOUNT_POINT "/images"
#define DOWNLOAD_DIRECTORY IMAGE_DIRECTORY "/Downloads"
#define DEFAULT_ALBUM_NAME "Default"

#include "board_hal.h"

#ifdef CONFIG_HAS_SDCARD
#define TEMP_MOUNT_POINT "/sdcard"
#else
#define TEMP_MOUNT_POINT "/ram"
#endif

#define CURRENT_UPLOAD_PATH TEMP_MOUNT_POINT "/.current.tmp"
#define CURRENT_JPG_PATH TEMP_MOUNT_POINT "/.current.jpg"
#define CURRENT_BMP_PATH TEMP_MOUNT_POINT "/.current.bmp"
#define CURRENT_PNG_PATH TEMP_MOUNT_POINT "/.current.png"
#define CURRENT_IMAGE_LINK TEMP_MOUNT_POINT "/.current.lnk"
#define CURRENT_CALIBRATION_PATH TEMP_MOUNT_POINT "/.calibration.png"

#ifdef DEBUG_DEEP_SLEEP_WAKE
#define AUTO_SLEEP_TIMEOUT_SEC 60
#else
#define AUTO_SLEEP_TIMEOUT_SEC 120
#endif

#define IMAGE_ROTATE_INTERVAL_SEC 3600

// WiFi
#define NVS_WIFI_SSID_KEY "wifi_ssid"
#define NVS_WIFI_PASS_KEY "wifi_pass"

// General
#define NVS_NAMESPACE "photoframe"
#define NVS_DEVICE_NAME_KEY "device_name"
#define NVS_TIMEZONE_KEY "timezone"
#define NVS_DISPLAY_ORIENTATION_KEY "disp_orient"
#define NVS_DISPLAY_ROTATION_DEG_KEY "disp_rot_deg"

// Auto Rotate
#define NVS_AUTO_ROTATE_KEY "auto_rotate"
#define NVS_ROTATE_INTERVAL_KEY "rotate_int"
#define NVS_AUTO_ROTATE_ALIGNED_KEY "ar_align"
#define NVS_ROTATION_MODE_KEY "rotation_mode"
#define NVS_SLEEP_SCHEDULE_ENABLED_KEY "sleep_sched_en"
#define NVS_SLEEP_SCHEDULE_START_KEY "sleep_start"
#define NVS_SLEEP_SCHEDULE_END_KEY "sleep_end"

// Auto Rotate - SDCard
#define NVS_SD_ROTATION_MODE_KEY "sd_rot_mode"
#define NVS_LAST_INDEX_KEY "last_idx"
#define NVS_ENABLED_ALBUMS_KEY "enabled_albums"

// Auto Rotate - URL
#define NVS_IMAGE_URL_KEY "image_url"
#define NVS_ACCESS_TOKEN_KEY "access_token"
#define NVS_HTTP_HEADER_KEY_KEY "http_hdr_key"
#define NVS_HTTP_HEADER_VALUE_KEY "http_hdr_val"
#define NVS_SAVE_DOWNLOADED_KEY "save_dl"

// Power
#define NVS_DEEP_SLEEP_KEY "deep_sleep"

// Home Assistant
#define NVS_HA_URL_KEY "ha_url"

// AI Generation
#define AI_API_KEY_MAX_LEN 256
#define AI_PROMPT_MAX_LEN 256
#define AI_MODEL_MAX_LEN 32
#define DEFAULT_AI_MODEL "gpt-image-1.5"
#define NVS_OPENAI_API_KEY_KEY "openai_key"
#define NVS_GOOGLE_API_KEY_KEY "google_key"
#define NVS_AI_PROMPT_KEY "ai_prompt"
#define NVS_AI_PROVIDER_KEY "ai_provider"
#define NVS_AI_MODEL_KEY "ai_model"

// OTA Configuration
#define GITHUB_API_URL "https://api.github.com/repos/aitjcize/esp32-photoframe/releases/latest"
#define OTA_CHECK_INTERVAL_MS (24 * 60 * 60 * 1000)  // 24 hours

#endif