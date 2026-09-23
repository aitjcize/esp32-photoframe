#include "chime.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "board_hal.h"
#include "chime_name.h"
#include "chime_presets.h"
#include "config.h"
#include "config_manager.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wav_pcm.h"

static const char *TAG = "chime";

bool chime_preset_is_valid(const char *preset)
{
    return chime_preset_id_is_valid(preset);
}

bool chime_source_is_valid(const char *source)
{
    return source && (strcmp(source, "preset") == 0 || strcmp(source, "wav") == 0 ||
                      strcmp(source, "uploaded") == 0);
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

size_t chime_cache_size(void)
{
    struct stat st;
    if (stat(CHIME_CACHE_PATH, &st) != 0 || st.st_size <= 0) {
        return 0;
    }
    return (size_t) st.st_size;
}

void chime_invalidate_cache(void)
{
    unlink(CHIME_CACHE_PATH);
    unlink(CHIME_CACHE_TMP_PATH);
}

esp_err_t chime_ensure_dir(void)
{
    struct stat st;
    if (stat(CHIME_DIRECTORY, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    if (mkdir(CHIME_DIRECTORY, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Failed to create %s: errno %d", CHIME_DIRECTORY, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void chime_uploaded_path(const char *filename, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s", CHIME_DIRECTORY, filename);
}

bool chime_uploaded_exists(const char *filename)
{
    if (!chime_filename_is_valid(filename)) {
        return false;
    }
    char path[256];
    chime_uploaded_path(filename, path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
           (size_t) st.st_size <= WAV_PCM_MAX_FILE_BYTES;
}

esp_err_t chime_validate_wav_file(const char *path)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((size_t) st.st_size > WAV_PCM_MAX_FILE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_FAIL;
    }
    wav_pcm_info_t info;
    int parse_ok = wav_pcm_parse_file(f, &info);
    fclose(f);
    if (parse_ok != 0 || !wav_pcm_is_supported(&info)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!wav_pcm_data_is_complete(&info, (size_t) st.st_size)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

int chime_list(chime_entry_t *out, int max)
{
    if (!out || max <= 0) {
        return 0;
    }
    DIR *dir = opendir(CHIME_DIRECTORY);
    if (!dir) {
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max) {
        if (!chime_filename_is_valid(entry->d_name)) {
            continue;
        }
        char path[256];
        chime_uploaded_path(entry->d_name, path, sizeof(path));
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            continue;
        }
        strncpy(out[count].name, entry->d_name, CHIME_FILENAME_MAX_LEN - 1);
        out[count].name[CHIME_FILENAME_MAX_LEN - 1] = '\0';
        out[count].size = (size_t) st.st_size;
        count++;
    }
    closedir(dir);
    return count;
}

esp_err_t chime_install_upload(const char *tmp_path, const char *original_filename, char *out_name,
                               size_t out_len)
{
    if (!tmp_path || !out_name || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t verr = chime_validate_wav_file(tmp_path);
    if (verr != ESP_OK) {
        return verr;
    }

    char name[CHIME_FILENAME_MAX_LEN];
    if (!chime_sanitize_filename(
            original_filename && original_filename[0] ? original_filename : "chime.wav", name,
            sizeof(name))) {
        return ESP_ERR_INVALID_ARG;
    }

    if (chime_ensure_dir() != ESP_OK) {
        return ESP_FAIL;
    }

    bool replacing = chime_uploaded_exists(name);
    if (!replacing) {
        chime_entry_t existing[CHIME_MAX_STORED + 1];
        if (chime_list(existing, CHIME_MAX_STORED + 1) >= CHIME_MAX_STORED) {
            ESP_LOGW(TAG, "Chime library full (%d files)", CHIME_MAX_STORED);
            return ESP_ERR_NO_MEM;
        }
    }

    char dest[256];
    chime_uploaded_path(name, dest, sizeof(dest));
    unlink(dest);
    if (rename(tmp_path, dest) != 0) {
        ESP_LOGE(TAG, "Failed to install uploaded chime as %s", dest);
        return ESP_FAIL;
    }

    strncpy(out_name, name, out_len - 1);
    out_name[out_len - 1] = '\0';
    ESP_LOGI(TAG, "Installed uploaded chime %s", name);
    return ESP_OK;
}

esp_err_t chime_delete(const char *filename)
{
    if (!chime_filename_is_valid(filename)) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[256];
    chime_uploaded_path(filename, path, sizeof(path));
    if (unlink(path) != 0) {
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    const char *active = config_manager_get_chime_file();
    if (active && strcmp(active, filename) == 0) {
        config_manager_set_chime_file("");
        if (config_manager_get_chime_source() == CHIME_SOURCE_UPLOADED) {
            config_manager_set_chime_source(CHIME_SOURCE_PRESET);
        }
    }
    return ESP_OK;
}

// Same budget as image fetch: ~2 MiB over flaky WiFi must not hang forever.
#define CHIME_DOWNLOAD_TIMEOUT_MS 120000

typedef struct {
    FILE *file;
    int total_read;
    bool too_large;
    bool have_content_length;
    long content_length;
} chime_download_ctx_t;

static esp_err_t chime_http_event(esp_http_client_event_t *evt)
{
    chime_download_ctx_t *ctx = (chime_download_ctx_t *) evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (strcasecmp(evt->header_key, "Content-Length") == 0) {
            char *end = NULL;
            long len = strtol(evt->header_value, &end, 10);
            if (end != evt->header_value && len >= 0) {
                ctx->have_content_length = true;
                ctx->content_length = len;
            }
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

    chime_download_ctx_t ctx = {.file = file,
                                .total_read = 0,
                                .too_large = false,
                                .have_content_length = false,
                                .content_length = -1};
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = CHIME_DOWNLOAD_TIMEOUT_MS,
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
    int http_len = esp_http_client_get_content_length(client);
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
    if (ctx.have_content_length && ctx.total_read != (int) ctx.content_length) {
        ESP_LOGW(TAG, "Chime WAV truncated (got %d of Content-Length %ld)", ctx.total_read,
                 ctx.content_length);
        unlink(CHIME_CACHE_TMP_PATH);
        return ESP_ERR_INVALID_SIZE;
    }
    if (http_len > 0 && ctx.total_read != http_len) {
        ESP_LOGW(TAG, "Chime WAV truncated (got %d of HTTP length %d)", ctx.total_read, http_len);
        unlink(CHIME_CACHE_TMP_PATH);
        return ESP_ERR_INVALID_SIZE;
    }

    struct stat st;
    if (stat(CHIME_CACHE_TMP_PATH, &st) != 0) {
        ESP_LOGW(TAG, "Chime WAV missing after download");
        unlink(CHIME_CACHE_TMP_PATH);
        return ESP_FAIL;
    }
    if (st.st_size != ctx.total_read) {
        ESP_LOGW(TAG, "Chime WAV size mismatch (file=%ld read=%d)", (long) st.st_size,
                 ctx.total_read);
        unlink(CHIME_CACHE_TMP_PATH);
        return ESP_FAIL;
    }

    FILE *check = fopen(CHIME_CACHE_TMP_PATH, "rb");
    if (!check) {
        unlink(CHIME_CACHE_TMP_PATH);
        return ESP_FAIL;
    }
    wav_pcm_info_t info;
    int parse_ok = wav_pcm_parse_file(check, &info);
    fclose(check);
    if (parse_ok != 0 || !wav_pcm_is_supported(&info) ||
        !wav_pcm_data_is_complete(&info, (size_t) st.st_size)) {
        ESP_LOGW(TAG, "Downloaded chime file is not a complete supported PCM WAV");
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

esp_err_t chime_pull_url(void)
{
    const char *url = config_manager_get_chime_url();
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    return chime_download_wav(url);
}

esp_err_t chime_play(chime_play_reason_t reason)
{
    return chime_play_detailed(reason, false, NULL);
}

esp_err_t chime_play_detailed(chime_play_reason_t reason, bool refresh, chime_play_result_t *out)
{
    if (out) {
        out->played = CHIME_PLAYED_NONE;
        out->fetched = false;
        out->fetch_err = ESP_OK;
    }

    if (!board_hal_has_speaker()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    chime_source_t source = config_manager_get_chime_source();
    if (source == CHIME_SOURCE_UPLOADED) {
        const char *file = config_manager_get_chime_file();
        if (file && chime_uploaded_exists(file)) {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", CHIME_DIRECTORY, file);
            esp_err_t perr = board_hal_play_wav_file(path);
            if (perr == ESP_OK) {
                if (out) {
                    out->played = CHIME_PLAYED_UPLOADED;
                }
                return ESP_OK;
            }
            ESP_LOGW(TAG, "Uploaded chime play failed, falling back to preset: %s",
                     esp_err_to_name(perr));
        }
        esp_err_t perr = board_hal_play_chime_preset(config_manager_get_chime_preset());
        if (out && perr == ESP_OK) {
            out->played = CHIME_PLAYED_PRESET;
        }
        return perr;
    }

    bool want_wav = (source == CHIME_SOURCE_WAV);
    const char *url = config_manager_get_chime_url();
    bool have_url = url && url[0];
    bool have_cache = chime_cache_exists();

    if (want_wav && have_url) {
        bool with_rotate = config_manager_get_chime_pull_mode() == CHIME_PULL_WITH_ROTATE;
        if (chime_should_fetch_url(reason, with_rotate, have_cache, refresh)) {
            esp_err_t ferr = chime_download_wav(url);
            if (out) {
                out->fetched = true;
                out->fetch_err = ferr;
            }
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
                if (out) {
                    out->played = CHIME_PLAYED_WAV;
                }
                return ESP_OK;
            }
            ESP_LOGW(TAG, "Chime WAV play failed, falling back to preset: %s",
                     esp_err_to_name(perr));
        }
    } else if (want_wav && out && !have_url) {
        out->fetch_err = ESP_ERR_INVALID_ARG;
    }

    esp_err_t perr = board_hal_play_chime_preset(config_manager_get_chime_preset());
    if (out && perr == ESP_OK) {
        out->played = CHIME_PLAYED_PRESET;
    }
    return perr;
}
