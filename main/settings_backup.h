#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "chime_name.h"

#ifdef __cplusplus
extern "C" {
#endif

// SD snapshot of the settings that NVS loses on a full flash at 0x0.
// Written to the same config/ folder as wifi.txt.
//
// Policy: NVS stays the fast runtime store. Every successful config save
// (config_manager_touch_config) also writes this file when an SD card is
// mounted. On boot, NVS is loaded first; if the SD file exists and NVS looks
// factory-fresh (none of the backed-up keys are present), the snapshot is
// imported and rewritten to NVS. WiFi stays on wifi.txt / NVS and is not
// stored here.
#ifndef FS_MOUNT_POINT
#define FS_MOUNT_POINT "/storage"
#endif

#define SETTINGS_BACKUP_DIR FS_MOUNT_POINT "/config"
#define SETTINGS_BACKUP_PATH SETTINGS_BACKUP_DIR "/settings.json"
#define SETTINGS_BACKUP_TMP_PATH SETTINGS_BACKUP_DIR "/settings.json.tmp"

// Match config.h — kept here so host tests do not need ESP headers.
#define SETTINGS_BACKUP_MAX_CRON_RULES 7
#define SETTINGS_BACKUP_CRON_RULE_MAX_LEN 64
#define SETTINGS_BACKUP_URL_MAX_LEN 256
#define SETTINGS_BACKUP_PRESET_MAX_LEN 16
#define SETTINGS_BACKUP_ENUM_MAX_LEN 16
#define SETTINGS_BACKUP_TZ_MAX_LEN 64
#define SETTINGS_BACKUP_NTP_MAX_LEN 128
#define SETTINGS_BACKUP_NAME_MAX_LEN 64
#define SETTINGS_BACKUP_VERSION 2

typedef struct {
    bool auto_rotate;
    bool deep_sleep_enabled;
    bool chime_enabled;
    bool debug_log_enabled;
    bool save_downloaded_images;
    int rotate_cron_count;
    int display_rotation_deg;
    char rotate_cron[SETTINGS_BACKUP_MAX_CRON_RULES][SETTINGS_BACKUP_CRON_RULE_MAX_LEN];
    char rotation_mode[SETTINGS_BACKUP_ENUM_MAX_LEN];
    char sd_rotation_mode[SETTINGS_BACKUP_ENUM_MAX_LEN];
    char display_orientation[SETTINGS_BACKUP_ENUM_MAX_LEN];
    char image_url[SETTINGS_BACKUP_URL_MAX_LEN];
    char ha_url[SETTINGS_BACKUP_URL_MAX_LEN];
    char timezone[SETTINGS_BACKUP_TZ_MAX_LEN];
    char ntp_server[SETTINGS_BACKUP_NTP_MAX_LEN];
    char device_name[SETTINGS_BACKUP_NAME_MAX_LEN];
    char chime_preset[SETTINGS_BACKUP_PRESET_MAX_LEN];
    char chime_url[SETTINGS_BACKUP_URL_MAX_LEN];
    char chime_source[SETTINGS_BACKUP_ENUM_MAX_LEN];
    char chime_pull_mode[SETTINGS_BACKUP_ENUM_MAX_LEN];
    char chime_play_when[SETTINGS_BACKUP_ENUM_MAX_LEN];
    char chime_file[CHIME_FILENAME_MAX_LEN];
    bool has_auto_rotate;
    bool has_rotate_cron;
    bool has_rotation_mode;
    bool has_sd_rotation_mode;
    bool has_image_url;
    bool has_deep_sleep_enabled;
    bool has_chime_enabled;
    bool has_chime_preset;
    bool has_chime_url;
    bool has_chime_source;
    bool has_chime_pull_mode;
    bool has_chime_play_when;
    bool has_chime_file;
    bool has_timezone;
    bool has_ntp_server;
    bool has_device_name;
    bool has_display_orientation;
    bool has_display_rotation_deg;
    bool has_debug_log_enabled;
    bool has_save_downloaded_images;
    bool has_ha_url;
} settings_backup_t;

// Pretty-print JSON into out (NUL-terminated). Returns bytes written excluding
// NUL, or -1 if the buffer is too small or `in` is NULL.
int settings_backup_serialize(const settings_backup_t *in, char *out, size_t out_len);

// Parse a snapshot. Unknown keys are ignored. Invalid values leave the
// corresponding has_* flag false. Returns false if the root object is missing.
bool settings_backup_parse(const char *json, settings_backup_t *out);

// Restore when an SD snapshot exists and NVS has none of the backed-up keys
// (typical after flashing a merged image at 0x0, which wipes NVS).
bool settings_backup_should_restore(bool nvs_has_backup_keys, bool sd_backup_present);

#ifdef __cplusplus
}
#endif
