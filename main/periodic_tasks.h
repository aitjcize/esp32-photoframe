#ifndef PERIODIC_TASKS_H
#define PERIODIC_TASKS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Periodic tasks names
#define SNTP_TASK_NAME "sntp_sync"
#define OTA_CHECK_TASK_NAME "ota_check"

/**
 * @brief Periodic task callback function type
 *
 * @return ESP_OK if task completed successfully, error code otherwise
 */
typedef esp_err_t (*periodic_task_callback_t)(void);

/**
 * @brief Initialize the periodic tasks manager
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t periodic_tasks_init(void);
// Scheduled wakes run tasks explicitly once; disable the hourly background timer.
esp_err_t periodic_tasks_stop_timer(void);

/**
 * @brief Register a periodic task
 *
 * @param task_name Unique name for the task (used as NVS key)
 * @param callback Function to call when task should run
 * @param interval_seconds How often to run the task (in seconds)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t periodic_tasks_register(const char *task_name, periodic_task_callback_t callback,
                                  uint32_t interval_seconds);

/**
 * @brief Check if any registered tasks should run and execute them
 *
 * This should be called periodically (e.g., on boot, after WiFi connects)
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t periodic_tasks_check_and_run(void);

/**
 * @brief Check if a specific task should run based on its interval
 *
 * @param task_name Name of the task to check
 * @return true if task should run, false otherwise
 */
bool periodic_tasks_should_run(const char *task_name);

/**
 * @brief Update the last run time for a task to current time
 *
 * @param task_name Name of the task
 * @return ESP_OK on success, error code on failure
 */
esp_err_t periodic_tasks_update_last_run(const char *task_name);

/**
 * @brief Get the last run time for a task
 *
 * @param task_name Name of the task
 * @param last_run_time Pointer to store the last run time (Unix timestamp)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t periodic_tasks_get_last_run(const char *task_name, int64_t *last_run_time);

/**
 * @brief Force a task to run on next check by clearing its last run time
 *
 * @param task_name Name of the task
 * @return ESP_OK on success, error code on failure
 */
esp_err_t periodic_tasks_force_run(const char *task_name);

#endif  // PERIODIC_TASKS_H
