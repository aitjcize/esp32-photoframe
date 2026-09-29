#include "crash_log.h"

#include <string.h>
#include <time.h>

#include "board_hal.h"
#include "config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH  // unless turned off in menuconfig
#include "esp_core_dump.h"
#endif

static const char *TAG = "crash_log";

static esp_err_t save_record(const crash_record_t *rec)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, NVS_LAST_CRASH_KEY, rec, sizeof(*rec));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool crash_log_load(crash_record_t *rec)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(*rec);
    esp_err_t err = nvs_get_blob(handle, NVS_LAST_CRASH_KEY, rec, &len);
    nvs_close(handle);
    return err == ESP_OK && len == sizeof(*rec) && rec->version == CRASH_RECORD_VERSION;
}

esp_err_t crash_log_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, NVS_LAST_CRASH_KEY);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    nvs_close(handle);
    return err;
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
void crash_log_capture(void)
{
    esp_err_t err = esp_core_dump_image_check();
    if (err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_INVALID_CRC) {
        // A torn write, or bytes an older layout left behind: nothing to
        // summarise, and it would be re-checked on every boot.
        ESP_LOGW(TAG, "COREDUMP: partition holds no valid dump (%s); erasing",
                 esp_err_to_name(err));
        esp_core_dump_image_erase();
        return;
    }
    if (err != ESP_OK) {
        return;  // none stored (or no coredump partition on an OTA'd layout)
    }

    size_t addr = 0, size = 0;
    esp_core_dump_image_get(&addr, &size);

    esp_core_dump_summary_t summary = {0};
    err = esp_core_dump_get_summary(&summary);
    if (err != ESP_OK || summary.exc_bt_info.depth == 0) {
        ESP_LOGE(TAG, "COREDUMP: %u bytes stored but could not be summarised (%s); left in flash",
                 (unsigned) size, esp_err_to_name(err));
        return;
    }

    crash_record_t rec = {
        .version = CRASH_RECORD_VERSION,
        .pc = summary.exc_pc,
        .bt_corrupted = summary.exc_bt_info.corrupted,
        .dump_size = size,
    };
    char details[sizeof(rec.reason)];
    if (esp_core_dump_get_panic_reason(details, sizeof(details)) != ESP_OK) {
        details[0] = '\0';
    }
    crash_record_set_reason(&rec, details, summary.ex_info.exc_cause, summary.ex_info.exc_vaddr);
    strlcpy(rec.task, summary.exc_task, sizeof(rec.task));
    for (uint32_t i = 0; i < summary.exc_bt_info.depth && i < CRASH_RECORD_BT_MAX; i++) {
        rec.bt[rec.bt_depth++] = summary.exc_bt_info.bt[i];
    }

    // The dump names its own build. Only vouch for the version string when
    // that is the build running now; a reflash after a crash loop isn't.
    strlcpy(rec.elf_sha, (const char *) summary.app_elf_sha256, sizeof(rec.elf_sha));
    char running_sha[sizeof(rec.elf_sha)];
    esp_app_get_elf_sha256(running_sha, sizeof(running_sha));
    if (strcmp(rec.elf_sha, running_sha) == 0) {
        strlcpy(rec.firmware, esp_app_get_description()->version, sizeof(rec.firmware));
    }

    // Approximate crash time: the RTC clock survives the panic reset.
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    if (tm.tm_year >= 2025 - 1900) {
        rec.found_at = now;
    }

    char line[CRASH_RECORD_LINE_MAX];
    crash_record_format(&rec, BOARD_HAL_NAME, line, sizeof(line));
    ESP_LOGE(TAG, "COREDUMP: %s", line);

    err = save_record(&rec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "COREDUMP: record not saved (%s); dump left in flash", esp_err_to_name(err));
        return;
    }
    esp_core_dump_image_erase();
}
#else
void crash_log_capture(void) {}
#endif
