#ifndef CONFIG_H
#define CONFIG_H

#include <driver/gpio.h>

// Uncomment to debug deep sleep wake
// #define DEBUG_DEEP_SLEEP_WAKE

typedef enum { ROTATION_MODE_SDCARD = 0, ROTATION_MODE_URL = 1 } rotation_mode_t;

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
#define IMAGE_DIRECTORY "/sdcard/images"
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

#define DISPLAY_WIDTH board_hal_get_display_width()
#define DISPLAY_HEIGHT board_hal_get_display_height()

#ifdef DEBUG_DEEP_SLEEP_WAKE
#define AUTO_SLEEP_TIMEOUT_SEC 60
#else
#define AUTO_SLEEP_TIMEOUT_SEC 120
#endif

#define IMAGE_ROTATE_INTERVAL_SEC 3600

#define NVS_NAMESPACE "photoframe"
#define NVS_DEVICE_NAME_KEY "device_name"
#define NVS_WIFI_SSID_KEY "wifi_ssid"
#define NVS_WIFI_PASS_KEY "wifi_pass"
#define NVS_ROTATE_INTERVAL_KEY "rotate_int"
#define NVS_AUTO_ROTATE_KEY "auto_rotate"
#define NVS_AUTO_ROTATE_ALIGNED_KEY "ar_align"
#define NVS_DISPLAY_ROTATION_DEG_KEY "disp_rot_deg"
#define NVS_DEEP_SLEEP_KEY "deep_sleep"
#define NVS_ENABLED_ALBUMS_KEY "enabled_albums"
#define NVS_IMAGE_URL_KEY "image_url"
#define NVS_ROTATION_MODE_KEY "rotation_mode"
#define NVS_SAVE_DOWNLOADED_KEY "save_dl"
#define NVS_HA_URL_KEY "ha_url"
#define NVS_DISPLAY_ORIENTATION_KEY "disp_orient"
#define NVS_SLEEP_SCHEDULE_ENABLED_KEY "sleep_sched_en"
#define NVS_SLEEP_SCHEDULE_START_KEY "sleep_start"
#define NVS_SLEEP_SCHEDULE_END_KEY "sleep_end"
#define NVS_TIMEZONE_KEY "timezone"
#define NVS_ACCESS_TOKEN_KEY "access_token"
#define NVS_HTTP_HEADER_KEY_KEY "http_hdr_key"
#define NVS_HTTP_HEADER_VALUE_KEY "http_hdr_val"

// OTA Configuration
#define GITHUB_API_URL "https://api.github.com/repos/aitjcize/esp32-photoframe/releases/latest"
#define OTA_CHECK_INTERVAL_MS (24 * 60 * 60 * 1000)  // 24 hours

#endif
