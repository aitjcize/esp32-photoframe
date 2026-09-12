#include "chime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board_hal.h"
#include "config.h"
#include "config_manager.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wav_pcm.h"

static const char *TAG = "chime";

static const char *k_presets[] = {"triad",     "dingdong", "doublebeep",
                                  "ascending", "softping", "alert"};

bool chime_preset_is_valid(const char *preset)
{
    if (!preset) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_presets) / sizeof(k_presets[0]); i++) {
        if (strcmp(preset, k_presets[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool chime_source_is_valid(const char *source)
{
    return source && (strcmp(source, "preset") == 0 || strcmp(source, "wav") == 0);
}

bool chime_pull_mode_is_valid(const char *mode)
{
    return mode && (strcmp(mode, "once") == 0 || strcmp(mode, "with_rotate") == 0);
}

bool chime_cache_exists(void)
{
    struct stat st;
    if (stat(CHIME_CACHE_PATH, &st) != 0 || st.st_size <= 0) {
        return false;
    }
    return (size_t) st.st_size <= WAV_PCM_MAX_FILE_BYTES;
}

void chime_invalidate_cache(void)
{
    unlink(CHIME_CACHE_PATH);
    unlink(CHIME_CACHE_TMP_PATH);
}

typedef struct {
    FILE *file;
    int total_read;
    bool too_large;
} chime_download_ctx_t;

static esp_err_t chime_http_event(esp_http_client_event_t *evt)
{
    chime_download_ctx_t *ctx = (chime_download_ctx_t *) evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (strcasecmp(evt->header_key, "Content-Length") == 0) {
            long len = strtol(evt->header_value, NULL, 10);
            if (len > (long) WAV_PCM_MAX_FILE_BYTES) {
                ESP_LOGW(TAG, "Chime WAV Content-Length %ld exceeds %u", len,
                         WAV_PCM_MAX_FILE_BYTES);
                ctx->too_large = true;
                return ESP_FAIL;
            }
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (!ctx->file) {
            break;
        }
        if (ctx->total_read + evt->data_len > (int) WAV_PCM_MAX_FILE_BYTES) {
            ESP_LOGW(TAG, "Chime WAV exceeded %u bytes, aborting", WAV_PCM_MAX_FILE_BYTES);
            ctx->too_large = true;
            return ESP_FAIL;
        }
        if (fwrite(evt->data, 1, evt->data_len, ctx->file) != (size_t) evt->data_len) {
            return ESP_FAIL;
        }
        ctx->total_read += evt->data_len;
        if ((ctx->total_read >> 15) != ((ctx->total_read - evt->data_len) >> 15)) {
            vTaskDelay(1);
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t chime_download_wav(const char *url)
{
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        ESP_LOGW(TAG, "Chime URL must be http(s): %s", url);
        return ESP_ERR_INVALID_ARG;
    }

    chime_invalidate_cache();

    FILE *file = fopen(CHIME_CACHE_TMP_PATH, "wb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open %s for chime WAV", CHIME_CACHE_TMP_PATH);
        return ESP_FAIL;
    }

    chime_download_ctx_t ctx = {.file = file, .total_read = 0, .too_large = false};
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 15000,
        .event_handler = chime_http_event,
        .user_data = &ctx,
        .max_redirection_count = 3,
        .user_agent = "ESP32-PhotoFrame-Chime",
    };

    ESP_LOGI(TAG, "Fetching chime WAV from %s", url);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(file);
        unlink(CHIME_CACHE_TMP_PATH);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    fclose(file);

    if (ctx.too_large) {
        unlink(CHIME_CACHE_TMP_PATH);
        return ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK || status != 200 || ctx.total_read <= 0) {
        ESP_LOGW(TAG, "Chime WAV fetch failed (err=%s status=%d bytes=%d)", esp_err_to_name(err),
                 status, ctx.total_read);
        unlink(CHIME_CACHE_TMP_PATH);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    FILE *check = fopen(CHIME_CACHE_TMP_PATH, "rb");
    if (!check) {
        unlink(CHIME_CACHE_TMP_PATH);
        return ESP_FAIL;
    }
    wav_pcm_info_t info;
    int parse_ok = wav_pcm_parse_file(check, &info);
    fclose(check);
    if (parse_ok != 0 || !wav_pcm_is_supported(&info)) {
        ESP_LOGW(TAG, "Downloaded chime file is not a supported PCM WAV");
        unlink(CHIME_CACHE_TMP_PATH);
        return ESP_ERR_NOT_SUPPORTED;
    }

    unlink(CHIME_CACHE_PATH);
    if (rename(CHIME_CACHE_TMP_PATH, CHIME_CACHE_PATH) != 0) {
        ESP_LOGE(TAG, "Failed to install chime WAV cache");
        unlink(CHIME_CACHE_TMP_PATH);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Cached chime WAV (%d bytes, %lu Hz)", ctx.total_read,
             (unsigned long) info.sample_rate);
    return ESP_OK;
}

esp_err_t chime_play(chime_play_reason_t reason)
{
    if (!board_hal_has_speaker()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    bool want_wav = (config_manager_get_chime_source() == CHIME_SOURCE_WAV);
    const char *url = config_manager_get_chime_url();
    bool have_url = url && url[0];
    bool have_cache = chime_cache_exists();

    if (want_wav && have_url) {
        bool should_fetch = false;
        if (reason == CHIME_PLAY_AFTER_DISPLAY &&
            config_manager_get_chime_pull_mode() == CHIME_PULL_WITH_ROTATE) {
            should_fetch = true;
        } else if (!have_cache) {
            // once: download on first need; preview does the same if no cache.
            should_fetch = true;
        }

        if (should_fetch) {
            esp_err_t ferr = chime_download_wav(url);
            if (ferr == ESP_OK) {
                have_cache = true;
            } else {
                ESP_LOGW(TAG, "Chime WAV fetch failed, will try cache/preset: %s",
                         esp_err_to_name(ferr));
                have_cache = chime_cache_exists();
            }
        }

        if (have_cache) {
            esp_err_t perr = board_hal_play_wav_file(CHIME_CACHE_PATH);
            if (perr == ESP_OK) {
                return ESP_OK;
            }
            ESP_LOGW(TAG, "Chime WAV play failed, falling back to preset: %s",
                     esp_err_to_name(perr));
        }
    }

    return board_hal_play_chime_preset(config_manager_get_chime_preset());
}
