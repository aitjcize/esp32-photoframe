#include "periodic_tasks.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "periodic_tasks";
#define PERIODIC_TASKS_NVS_NAMESPACE "periodic"
#define MAX_TASKS 16
#define PERIODIC_CHECK_INTERVAL_MS (60 * 60 * 1000)  // Check every hour

typedef struct {
    char task_name[32];
    periodic_task_callback_t callback;
    uint32_t interval_seconds;
    bool registered;
} periodic_task_t;

static periodic_task_t tasks[MAX_TASKS];
static int task_count = 0;
static esp_timer_handle_t periodic_check_timer = NULL;

static void periodic_check_timer_callback(void *arg)
{
    ESP_LOGI(TAG, "Periodic check timer triggered");
    periodic_tasks_check_and_run();
}

esp_err_t periodic_tasks_init(void)
{
    memset(tasks, 0, sizeof(tasks));
    task_count = 0;

    // Create periodic timer to check tasks every hour
    const esp_timer_create_args_t timer_args = {.callback = &periodic_check_timer_callback,
                                                .name = "periodic_check_timer"};

    esp_err_t err = esp_timer_create(&timer_args, &periodic_check_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create periodic check timer: %s", esp_err_to_name(err));
        return err;
    }

    // Start periodic timer (check every hour)
    err = esp_timer_start_periodic(periodic_check_timer,
                                   (uint64_t) PERIODIC_CHECK_INTERVAL_MS * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start periodic check timer: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Periodic tasks manager initialized with hourly timer");
    return ESP_OK;
}

esp_err_t periodic_tasks_stop_timer(void)
{
    return esp_timer_stop(periodic_check_timer);
}

esp_err_t periodic_tasks_register(const char *task_name, periodic_task_callback_t callback,
                                  uint32_t interval_seconds)
{
    if (task_count >= MAX_TASKS) {
        ESP_LOGE(TAG, "Maximum number of tasks (%d) reached", MAX_TASKS);
        return ESP_ERR_NO_MEM;
    }

    if (task_name == NULL || callback == NULL) {
        ESP_LOGE(TAG, "Invalid task parameters");
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(task_name) >= sizeof(tasks[0].task_name)) {
        ESP_LOGE(TAG, "Task name too long: %s", task_name);
        return ESP_ERR_INVALID_ARG;
    }

    // Check if task already registered
    for (int i = 0; i < task_count; i++) {
        if (strcmp(tasks[i].task_name, task_name) == 0) {
            ESP_LOGW(TAG, "Task '%s' already registered, updating", task_name);
            tasks[i].callback = callback;
            tasks[i].interval_seconds = interval_seconds;
            return ESP_OK;
        }
    }

    // Register new task
    strncpy(tasks[task_count].task_name, task_name, sizeof(tasks[task_count].task_name) - 1);
    tasks[task_count].task_name[sizeof(tasks[task_count].task_name) - 1] = '\0';
    tasks[task_count].callback = callback;
    tasks[task_count].interval_seconds = interval_seconds;
    tasks[task_count].registered = true;
    task_count++;

    ESP_LOGI(TAG, "Registered task '%s' with interval %lu seconds", task_name, interval_seconds);
    return ESP_OK;
}

bool periodic_tasks_should_run(const char *task_name)
{
    if (task_name == NULL) {
        return false;
    }

    // Find the task to get its interval
    uint32_t interval_seconds = 0;
    for (int i = 0; i < task_count; i++) {
        if (strcmp(tasks[i].task_name, task_name) == 0) {
            interval_seconds = tasks[i].interval_seconds;
            break;
        }
    }

    if (interval_seconds == 0) {
        ESP_LOGW(TAG, "Task '%s' not found", task_name);
        return false;
    }

    // Get last run time from NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(PERIODIC_TASKS_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        // If NVS doesn't exist, task should run
        ESP_LOGD(TAG, "NVS namespace not found, task '%s' should run", task_name);
        return true;
    }

    int64_t last_run_time = 0;
    err = nvs_get_i64(nvs_handle, task_name, &last_run_time);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        // If no last run time, task should run
        ESP_LOGD(TAG, "No last run time for task '%s', should run", task_name);
        return true;
    }

    // Get current time
    time_t now;
    time(&now);
    int64_t current_time = (int64_t) now;

    // Check if system time is not set (before year 2020)
    if (current_time < 1577836800) {  // Jan 1, 2020
        ESP_LOGW(TAG, "System time not set, cannot determine if task '%s' should run", task_name);
        return false;
    }

    // Check if enough time has passed
    int64_t time_since_last_run = current_time - last_run_time;
    bool should_run = time_since_last_run >= (int64_t) interval_seconds;

    ESP_LOGD(TAG, "Task '%s': last run %lld, current %lld, elapsed %lld/%lu seconds, should_run=%d",
             task_name, last_run_time, current_time, time_since_last_run, interval_seconds,
             should_run);

    return should_run;
}

esp_err_t periodic_tasks_update_last_run(const char *task_name)
{
    if (task_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(PERIODIC_TASKS_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    time_t now;
    time(&now);
    int64_t current_time = (int64_t) now;

    err = nvs_set_i64(nvs_handle, task_name, current_time);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set last run time for '%s': %s", task_name, esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(TAG, "Updated last run time for task '%s' to %lld", task_name, current_time);
    return ESP_OK;
}

esp_err_t periodic_tasks_get_last_run(const char *task_name, int64_t *last_run_time)
{
    if (task_name == NULL || last_run_time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(PERIODIC_TASKS_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        *last_run_time = 0;
        return err;
    }

    err = nvs_get_i64(nvs_handle, task_name, last_run_time);
    nvs_close(nvs_handle);

    return err;
}

esp_err_t periodic_tasks_force_run(const char *task_name)
{
    if (task_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Clear the last run time from NVS to force task to run on next check
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(PERIODIC_TASKS_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_erase_key(nvs_handle, task_name);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to erase last run time for '%s': %s", task_name,
                 esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Cleared last run time for task '%s' - will run on next check", task_name);
    return ESP_OK;
}

esp_err_t periodic_tasks_check_and_run(void)
{
    ESP_LOGI(TAG, "Checking %d registered tasks", task_count);

    for (int i = 0; i < task_count; i++) {
        if (!tasks[i].registered || tasks[i].callback == NULL) {
            continue;
        }

        if (periodic_tasks_should_run(tasks[i].task_name)) {
            ESP_LOGI(TAG, "Running task '%s'", tasks[i].task_name);
            esp_err_t err = tasks[i].callback();

            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Task '%s' completed successfully", tasks[i].task_name);
                periodic_tasks_update_last_run(tasks[i].task_name);
            } else {
                ESP_LOGW(TAG, "Task '%s' failed: %s", tasks[i].task_name, esp_err_to_name(err));
            }
        } else {
            ESP_LOGD(TAG, "Task '%s' does not need to run yet", tasks[i].task_name);
        }
    }

    return ESP_OK;
}
