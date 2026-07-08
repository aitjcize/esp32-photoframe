#include "debug_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "config_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "storage.h"

static const char *TAG = "debug_log";

#define DEBUG_LOG_PATH FS_MOUNT_POINT "/.debug.log"
#define DEBUG_LOG_OLD_PATH FS_MOUNT_POINT "/.debug.log.old"

// Lines per generation; with the current + previous generation on disk the
// total is bounded to 2x this while always retaining at least the most
// recent DEBUG_LOG_MAX_LINES lines.
#define DEBUG_LOG_MAX_LINES 5000

// RAM ring buffer between the vprintf hook (any task) and the writer task.
// Writing to the filesystem directly from the hook could deadlock: a driver
// may log while its task already holds the filesystem lock.
#define RING_SIZE 8192
#define LINE_BUF_SIZE 512

// fsync cadence: cheap enough to not stall logging, tight enough that a
// crash loses at most a few dozen lines.
#define FSYNC_LINE_INTERVAL 32
#define FSYNC_TICK_INTERVAL pdMS_TO_TICKS(5000)

static vprintf_like_t s_orig_vprintf;
static SemaphoreHandle_t s_file_mutex;  // protects s_fp, s_line_count, rotation
static SemaphoreHandle_t s_data_sem;    // wakes the writer task
static portMUX_TYPE s_ring_lock = portMUX_INITIALIZER_UNLOCKED;

static char *s_ring;
static size_t s_ring_head;  // next write index
static size_t s_ring_tail;  // next read index
static size_t s_ring_dropped;

static TaskHandle_t s_writer_task;
static volatile bool s_writer_run;
static volatile bool s_capture;

static FILE *s_fp;
static int s_line_count;
static int s_lines_since_sync;
static TickType_t s_last_sync_tick;

const char *debug_log_current_path(void)
{
    return DEBUG_LOG_PATH;
}

const char *debug_log_old_path(void)
{
    return DEBUG_LOG_OLD_PATH;
}

static size_t ring_used(void)
{
    return (s_ring_head + RING_SIZE - s_ring_tail) % RING_SIZE;
}

static void ring_write(const char *data, size_t len)
{
    portENTER_CRITICAL(&s_ring_lock);
    if (!s_ring || len >= RING_SIZE - ring_used()) {
        s_ring_dropped++;
        portEXIT_CRITICAL(&s_ring_lock);
        return;
    }
    for (size_t i = 0; i < len; i++) {
        s_ring[s_ring_head] = data[i];
        s_ring_head = (s_ring_head + 1) % RING_SIZE;
    }
    portEXIT_CRITICAL(&s_ring_lock);
}

static size_t ring_read(char *out, size_t max_len)
{
    portENTER_CRITICAL(&s_ring_lock);
    size_t n = 0;
    while (s_ring && n < max_len && s_ring_tail != s_ring_head) {
        out[n++] = s_ring[s_ring_tail];
        s_ring_tail = (s_ring_tail + 1) % RING_SIZE;
    }
    portEXIT_CRITICAL(&s_ring_lock);
    return n;
}

static int debug_log_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);

    int ret = s_orig_vprintf ? s_orig_vprintf(fmt, args) : vprintf(fmt, args);

    if (s_capture && !xPortInIsrContext()) {
        char buf[LINE_BUF_SIZE];
        int len = vsnprintf(buf, sizeof(buf), fmt, copy);
        if (len > 0) {
            if (len >= (int) sizeof(buf)) {
                len = sizeof(buf) - 1;
            }
            ring_write(buf, (size_t) len);
            xSemaphoreGive(s_data_sem);
        }
    }

    va_end(copy);
    return ret;
}

// Count lines in an existing log so rotation carries across reboots.
static int count_lines(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return 0;
    }
    int lines = 0;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (c == '\n') {
            lines++;
        }
    }
    fclose(fp);
    return lines;
}

// Caller must hold s_file_mutex.
static void rotate_locked(void)
{
    if (s_fp) {
        fclose(s_fp);
        s_fp = NULL;
    }
    remove(DEBUG_LOG_OLD_PATH);
    rename(DEBUG_LOG_PATH, DEBUG_LOG_OLD_PATH);
    s_fp = fopen(DEBUG_LOG_PATH, "w");
    s_line_count = 0;
}

// Caller must hold s_file_mutex.
static void write_file_locked(const char *data, size_t len)
{
    if (!s_fp) {
        return;
    }
    fwrite(data, 1, len, s_fp);
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            s_line_count++;
            s_lines_since_sync++;
        }
    }
    if (s_line_count >= DEBUG_LOG_MAX_LINES) {
        rotate_locked();
    }
}

// Caller must hold s_file_mutex.
static void sync_locked(void)
{
    if (s_fp && s_lines_since_sync > 0) {
        fflush(s_fp);
        fsync(fileno(s_fp));
        s_lines_since_sync = 0;
        s_last_sync_tick = xTaskGetTickCount();
    }
}

// Caller must hold s_file_mutex. Drains the RAM ring into the file.
static void drain_locked(void)
{
    char buf[256];
    size_t n;
    while ((n = ring_read(buf, sizeof(buf))) > 0) {
        write_file_locked(buf, n);
    }

    size_t dropped;
    portENTER_CRITICAL(&s_ring_lock);
    dropped = s_ring_dropped;
    s_ring_dropped = 0;
    portEXIT_CRITICAL(&s_ring_lock);
    if (dropped > 0 && s_fp) {
        int len =
            snprintf(buf, sizeof(buf), "[debug_log: %u message(s) dropped]\n", (unsigned) dropped);
        write_file_locked(buf, (size_t) len);
    }
}

static void writer_task(void *arg)
{
    while (s_writer_run) {
        xSemaphoreTake(s_data_sem, pdMS_TO_TICKS(1000));

        xSemaphoreTake(s_file_mutex, portMAX_DELAY);
        drain_locked();
        if (s_lines_since_sync >= FSYNC_LINE_INTERVAL ||
            (s_lines_since_sync > 0 &&
             (xTaskGetTickCount() - s_last_sync_tick) >= FSYNC_TICK_INTERVAL)) {
            sync_locked();
        }
        xSemaphoreGive(s_file_mutex);
    }

    s_writer_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t capture_start(void)
{
    if (s_capture) {
        return ESP_OK;
    }
    if (!storage_has_persistent_storage()) {
        ESP_LOGW(TAG, "No persistent storage, debug log capture unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    s_ring = malloc(RING_SIZE);
    if (!s_ring) {
        return ESP_ERR_NO_MEM;
    }
    s_ring_head = 0;
    s_ring_tail = 0;
    s_ring_dropped = 0;

    xSemaphoreTake(s_file_mutex, portMAX_DELAY);
    s_line_count = count_lines(DEBUG_LOG_PATH);
    s_fp = fopen(DEBUG_LOG_PATH, "a");
    if (s_fp && s_line_count >= DEBUG_LOG_MAX_LINES) {
        rotate_locked();
    }
    s_lines_since_sync = 0;
    s_last_sync_tick = xTaskGetTickCount();
    bool file_ok = (s_fp != NULL);
    xSemaphoreGive(s_file_mutex);

    if (!file_ok) {
        ESP_LOGE(TAG, "Failed to open %s", DEBUG_LOG_PATH);
        free(s_ring);
        s_ring = NULL;
        return ESP_FAIL;
    }

    s_writer_run = true;
    if (xTaskCreate(writer_task, "debug_log", 4096, NULL, tskIDLE_PRIORITY + 1, &s_writer_task) !=
        pdPASS) {
        s_writer_run = false;
        xSemaphoreTake(s_file_mutex, portMAX_DELAY);
        fclose(s_fp);
        s_fp = NULL;
        xSemaphoreGive(s_file_mutex);
        free(s_ring);
        s_ring = NULL;
        return ESP_FAIL;
    }

    s_orig_vprintf = esp_log_set_vprintf(debug_log_vprintf);
    s_capture = true;
    ESP_LOGI(TAG, "Debug log capture started (%d lines at %s)", s_line_count, DEBUG_LOG_PATH);
    return ESP_OK;
}

static void capture_stop(void)
{
    if (!s_capture) {
        return;
    }

    ESP_LOGI(TAG, "Stopping debug log capture");
    s_capture = false;
    esp_log_set_vprintf(s_orig_vprintf);
    s_orig_vprintf = NULL;

    // Let in-flight hook calls that already passed the s_capture check finish
    // before tearing the ring down.
    vTaskDelay(pdMS_TO_TICKS(50));

    s_writer_run = false;
    xSemaphoreGive(s_data_sem);
    while (s_writer_task) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    xSemaphoreTake(s_file_mutex, portMAX_DELAY);
    drain_locked();
    sync_locked();
    if (s_fp) {
        fclose(s_fp);
        s_fp = NULL;
    }
    char *ring = s_ring;
    portENTER_CRITICAL(&s_ring_lock);
    s_ring = NULL;
    portEXIT_CRITICAL(&s_ring_lock);
    free(ring);
    xSemaphoreGive(s_file_mutex);
}

esp_err_t debug_log_init(void)
{
    if (!s_file_mutex) {
        s_file_mutex = xSemaphoreCreateMutex();
        s_data_sem = xSemaphoreCreateBinary();
        if (!s_file_mutex || !s_data_sem) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (config_manager_get_debug_log_enabled()) {
        return capture_start();
    }
    return ESP_OK;
}

void debug_log_set_enabled(bool enabled)
{
    config_manager_set_debug_log_enabled(enabled);
    if (enabled) {
        capture_start();
    } else {
        capture_stop();
    }
}

bool debug_log_get_enabled(void)
{
    return config_manager_get_debug_log_enabled();
}

void debug_log_flush(void)
{
    // Stops capture and closes the file so a following storage unmount /
    // deep sleep leaves the filesystem clean. Config stays enabled, so
    // capture resumes on the next boot.
    capture_stop();
}

void debug_log_lock(void)
{
    if (!s_file_mutex) {
        return;
    }
    // Nudge the writer so lines still in the RAM ring land in the file before
    // we snapshot it for download.
    if (s_capture) {
        xSemaphoreGive(s_data_sem);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    xSemaphoreTake(s_file_mutex, portMAX_DELAY);
    drain_locked();
    sync_locked();
}

void debug_log_unlock(void)
{
    if (!s_file_mutex) {
        return;
    }
    xSemaphoreGive(s_file_mutex);
}
