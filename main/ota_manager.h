#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_CHECKING,
    OTA_STATE_UPDATE_AVAILABLE,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_INSTALLING,
    OTA_STATE_SUCCESS,
    OTA_STATE_ERROR
} ota_state_t;

typedef struct {
    ota_state_t state;
    char current_version[32];
    char latest_version[32];
    char error_message[128];
    int progress_percent;
} ota_status_t;

esp_err_t ota_manager_init(void);
esp_err_t ota_check_for_update(bool *update_available);
esp_err_t ota_start_update(void);
void ota_get_status(ota_status_t *status);
const char *ota_get_current_version(void);

#endif  // OTA_MANAGER_H
