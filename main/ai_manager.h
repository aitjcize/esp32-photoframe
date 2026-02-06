#ifndef AI_MANAGER_H
#define AI_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    AI_STATUS_IDLE,
    AI_STATUS_GENERATING,
    AI_STATUS_DOWNLOADING,
    AI_STATUS_COMPLETE,
    AI_STATUS_ERROR
} ai_generation_status_t;

/**
 * @brief Initialize the AI manager
 */
esp_err_t ai_manager_init(void);

/**
 * @brief Trigger AI image generation
 *
 * @param prompt_override Optional prompt to override the configured one (used for manual
 * generation)
 * @return ESP_OK if started, ESP_FAIL if already busy or error
 */
esp_err_t ai_manager_generate(const char *prompt_override);

/**
 * @brief Get current generation status
 */
ai_generation_status_t ai_manager_get_status(void);

/**
 * @brief Get path to the last generated image
 */
const char *ai_manager_get_last_image_path(void);

/**
 * @brief Get last error message
 */
const char *ai_manager_get_last_error(void);

#endif
