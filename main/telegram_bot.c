#include "telegram_bot.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "album_manager.h"
#include "battery_history.h"
#include "board_hal.h"
#include "cJSON.h"
#include "config.h"
#include "config_manager.h"
#include "cron.h"
#include "display_manager.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "history_manager.h"
#include "image_processor.h"
#include "power_manager.h"
#include "processing_settings.h"
#include "storage.h"
#include "wifi_manager.h"

static const char *TAG = "telegram_bot";

// Cap on the accumulated getUpdates/getFile response body. A batch of
// TELEGRAM_MAX_UPDATES_PER_POLL updates comfortably fits well under this.
#define TELEGRAM_MAX_RESPONSE_BYTES (256 * 1024)

// Pending "/" commands queued by the last poll, executed (and replied to)
// after the image has been displayed but before deep sleep. Reset on every
// boot (deep sleep restarts the app), which matches the intended lifetime:
// queue -> run -> sleep, once per wake.
static char s_pending_commands[TELEGRAM_MAX_PENDING_COMMANDS][TELEGRAM_COMMAND_MAX_LEN];
static int s_pending_command_count = 0;

// ----------------------------------------------------------------------------
// Small HTTP helpers
// ----------------------------------------------------------------------------

// Growing in-memory buffer used to capture GET response bodies (getUpdates,
// getFile). Capped at TELEGRAM_MAX_RESPONSE_BYTES to bound worst-case heap use.
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool overflow;
} http_body_buf_t;

static esp_err_t body_capture_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    http_body_buf_t *ctx = (http_body_buf_t *) evt->user_data;
    if (ctx->overflow || evt->data_len <= 0) {
        return ESP_OK;
    }

    size_t need = ctx->len + (size_t) evt->data_len + 1;
    if (need > TELEGRAM_MAX_RESPONSE_BYTES) {
        ESP_LOGW(TAG, "Response body exceeds %d bytes cap, truncating", TELEGRAM_MAX_RESPONSE_BYTES);
        ctx->overflow = true;
        return ESP_OK;
    }
    if (need > ctx->cap) {
        size_t new_cap = ctx->cap ? ctx->cap * 2 : 4096;
        while (new_cap < need) {
            new_cap *= 2;
        }
        char *grown = realloc(ctx->buf, new_cap);
        if (!grown) {
            ESP_LOGE(TAG, "Out of memory growing response buffer to %zu bytes", new_cap);
            ctx->overflow = true;
            return ESP_OK;
        }
        ctx->buf = grown;
        ctx->cap = new_cap;
    }

    memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
    ctx->len += evt->data_len;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

// Transient TLS/network hiccups (e.g. a failed mbedtls handshake against
// api.telegram.org) are common enough on ESP32 to warrant a couple of quick
// retries, mirroring the retry loop already used for image-server fetches in
// utils.c - a single blip shouldn't cost the whole poll cycle (missed
// images/commands until the next wake).
#define TELEGRAM_HTTP_RETRY_COUNT 3
#define TELEGRAM_HTTP_RETRY_DELAY_MS 1500

// Every Telegram API URL embeds the bot token as ".../bot<TOKEN>/...". NEVER
// log a raw URL - always redact through this first (a device log, including
// the downloadable debug log, is not a safe place for a live bot token).
static void redact_url_for_log(const char *url, char *out, size_t out_len)
{
    const char *marker = "/bot";
    const char *pos = strstr(url, marker);
    if (!pos) {
        strncpy(out, url, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }

    size_t prefix_len = (size_t) (pos - url) + strlen(marker);
    if (prefix_len >= out_len) {
        out[0] = '\0';
        return;
    }
    const char *token_start = pos + strlen(marker);
    const char *next_slash = strchr(token_start, '/');

    memcpy(out, url, prefix_len);
    out[prefix_len] = '\0';
    snprintf(out + prefix_len, out_len - prefix_len, "***%s", next_slash ? next_slash : "");
}

// Performs a GET request (with retry on transient failure) and returns the
// response body (caller frees with free()). *out_body is NULL on any failure.
static esp_err_t telegram_http_get(const char *url, int timeout_ms, char **out_body,
                                   size_t *out_len)
{
    *out_body = NULL;
    if (out_len) {
        *out_len = 0;
    }

    esp_err_t last_err = ESP_FAIL;
    char safe_url[160];
    redact_url_for_log(url, safe_url, sizeof(safe_url));

    for (int attempt = 1; attempt <= TELEGRAM_HTTP_RETRY_COUNT; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "Retrying GET (%d/%d) after %d ms...", attempt, TELEGRAM_HTTP_RETRY_COUNT,
                     TELEGRAM_HTTP_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(TELEGRAM_HTTP_RETRY_DELAY_MS));
        }

        http_body_buf_t ctx = {0};

        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = timeout_ms,
            .event_handler = body_capture_handler,
            .user_data = &ctx,
            .buffer_size = 2048,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client for GET %s", safe_url);
            free(ctx.buf);
            last_err = ESP_FAIL;
            continue;
        }

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GET %s failed: %s", safe_url, esp_err_to_name(err));
            free(ctx.buf);
            last_err = err;
            continue;
        }
        if (status != 200 || ctx.overflow || !ctx.buf) {
            ESP_LOGE(TAG, "GET %s returned HTTP %d%s", safe_url, status,
                     ctx.overflow ? " (truncated)" : "");
            free(ctx.buf);
            last_err = ESP_FAIL;
            continue;
        }

        *out_body = ctx.buf;
        if (out_len) {
            *out_len = ctx.len;
        }
        return ESP_OK;
    }

    return last_err;
}

// Downloads raw bytes (a Telegram file) straight to a local file.
typedef struct {
    FILE *file;
    int total_bytes;
} file_download_ctx_t;

static esp_err_t file_download_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    file_download_ctx_t *ctx = (file_download_ctx_t *) evt->user_data;
    if (ctx->file && evt->data_len > 0) {
        fwrite(evt->data, 1, evt->data_len, ctx->file);
        ctx->total_bytes += evt->data_len;
    }
    return ESP_OK;
}

// Retries a couple of times on transient failure before letting the
// progressive size fallback (in download_photo_with_fallback) give up on this
// size entirely - a quick retry at the preferred size beats silently
// dropping to a lower-quality one over a one-off network blip.
#define TELEGRAM_DOWNLOAD_RETRY_COUNT 2
#define TELEGRAM_DOWNLOAD_RETRY_DELAY_MS 1500

static esp_err_t telegram_download_to_file(const char *url, const char *local_path)
{
    for (int attempt = 1; attempt <= TELEGRAM_DOWNLOAD_RETRY_COUNT; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "Retrying download (%d/%d) after %d ms...", attempt,
                     TELEGRAM_DOWNLOAD_RETRY_COUNT, TELEGRAM_DOWNLOAD_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(TELEGRAM_DOWNLOAD_RETRY_DELAY_MS));
        }

        FILE *f = fopen(local_path, "wb");
        if (!f) {
            ESP_LOGE(TAG, "Failed to open %s for writing", local_path);
            return ESP_FAIL;
        }

        file_download_ctx_t ctx = {.file = f, .total_bytes = 0};
        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = TELEGRAM_HTTP_TIMEOUT_MS,
            .event_handler = file_download_handler,
            .user_data = &ctx,
            .buffer_size = 4096,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            fclose(f);
            unlink(local_path);
            continue;
        }

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        fclose(f);
        esp_http_client_cleanup(client);

        if (err != ESP_OK || status != 200 || ctx.total_bytes <= 0) {
            ESP_LOGW(TAG, "File download failed (err=%s, status=%d, bytes=%d)", esp_err_to_name(err),
                     status, ctx.total_bytes);
            unlink(local_path);
            continue;
        }

        ESP_LOGI(TAG, "Downloaded %d bytes to %s", ctx.total_bytes, local_path);
        return ESP_OK;
    }

    return ESP_FAIL;
}

// ----------------------------------------------------------------------------
// Telegram Bot API method helpers
// ----------------------------------------------------------------------------

static void build_api_url(const char *method, char *out, size_t out_len)
{
    snprintf(out, out_len, TELEGRAM_API_BASE_FMT, config_manager_get_telegram_bot_token(), method);
}

// POSTs a JSON body to a Telegram API method with retry on transient failure.
// Takes ownership of `body` (always deletes it).
static esp_err_t telegram_api_post(const char *method, cJSON *body)
{
    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    char url[256];
    build_api_url(method, url, sizeof(url));

    esp_err_t result = ESP_FAIL;
    for (int attempt = 1; attempt <= TELEGRAM_HTTP_RETRY_COUNT; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "Retrying %s (%d/%d) after %d ms...", method, attempt,
                     TELEGRAM_HTTP_RETRY_COUNT, TELEGRAM_HTTP_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(TELEGRAM_HTTP_RETRY_DELAY_MS));
        }

        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = TELEGRAM_HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            continue;
        }

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, payload, strlen(payload));

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK || status != 200) {
            ESP_LOGW(TAG, "%s failed (err=%s, status=%d)", method, esp_err_to_name(err), status);
            continue;
        }
        result = ESP_OK;
        break;
    }

    free(payload);
    return result;
}

esp_err_t telegram_bot_send_message(const char *text)
{
    if (!config_manager_telegram_is_configured() || !text) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *body = cJSON_CreateObject();
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "chat_id", config_manager_get_telegram_chat_id());
    cJSON_AddStringToObject(body, "text", text);
    return telegram_api_post("sendMessage", body);
}

// Same as telegram_bot_send_message(), but threaded as a reply to a specific
// message (reply_to_message_id <= 0 means "no threading").
static esp_err_t telegram_bot_send_message_reply(const char *text, int64_t reply_to_message_id)
{
    if (!config_manager_telegram_is_configured() || !text) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *body = cJSON_CreateObject();
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "chat_id", config_manager_get_telegram_chat_id());
    cJSON_AddStringToObject(body, "text", text);
    if (reply_to_message_id > 0) {
        cJSON_AddNumberToObject(body, "reply_to_message_id", (double) reply_to_message_id);
    }
    return telegram_api_post("sendMessage", body);
}

// Sends a photo the bot already knows about (by file_id - no re-upload needed)
// as a captioned reply to a specific message. Used for the per-image "saved"
// confirmation, echoing back the smallest available Telegram-hosted size.
static esp_err_t telegram_bot_send_photo_reply(const char *file_id, const char *caption,
                                               int64_t reply_to_message_id)
{
    if (!config_manager_telegram_is_configured() || !file_id || file_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *body = cJSON_CreateObject();
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "chat_id", config_manager_get_telegram_chat_id());
    cJSON_AddStringToObject(body, "photo", file_id);
    if (caption) {
        cJSON_AddStringToObject(body, "caption", caption);
    }
    if (reply_to_message_id > 0) {
        cJSON_AddNumberToObject(body, "reply_to_message_id", (double) reply_to_message_id);
    }
    return telegram_api_post("sendPhoto", body);
}

// Resolves a Telegram file_id to a downloadable file_path via getFile.
static esp_err_t telegram_get_file_path(const char *file_id, char *out_path, size_t out_len)
{
    char base[256];
    build_api_url("getFile", base, sizeof(base));
    char url[320];
    snprintf(url, sizeof(url), "%s?file_id=%s", base, file_id);

    char *body = NULL;
    esp_err_t err = telegram_http_get(url, TELEGRAM_HTTP_TIMEOUT_MS, &body, NULL);
    if (err != ESP_OK || !body) {
        free(body);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGE(TAG, "getFile: failed to parse JSON response");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (ok && cJSON_IsTrue(ok) && result) {
        cJSON *fp = cJSON_GetObjectItem(result, "file_path");
        if (fp && cJSON_IsString(fp)) {
            strncpy(out_path, fp->valuestring, out_len - 1);
            out_path[out_len - 1] = '\0';
            ret = ESP_OK;
        }
    } else {
        cJSON *desc = cJSON_GetObjectItem(root, "description");
        ESP_LOGW(TAG, "getFile failed: %s",
                 (desc && cJSON_IsString(desc)) ? desc->valuestring : "unknown error");
    }
    cJSON_Delete(root);
    return ret;
}

static esp_err_t telegram_download_file_id(const char *file_id, const char *local_path)
{
    char tg_file_path[256];
    if (telegram_get_file_path(file_id, tg_file_path, sizeof(tg_file_path)) != ESP_OK) {
        return ESP_FAIL;
    }

    char url[400];
    snprintf(url, sizeof(url), "https://" TELEGRAM_API_HOST "/file/bot%s/%s",
             config_manager_get_telegram_bot_token(), tg_file_path);
    return telegram_download_to_file(url, local_path);
}

// ----------------------------------------------------------------------------
// Progressive-JPEG detection
// ----------------------------------------------------------------------------

// Telegram re-encodes compressed "photo" messages as progressive JPEG, which
// this firmware's JPEG decoder (tjpgd-based) cannot decode (only baseline/
// sequential SOF0/SOF1). Scan the JPEG marker stream for SOF0/1 (baseline,
// OK) vs SOF2/3 (progressive/lossless, unsupported) so a bad download can be
// treated as a failure and trigger the same progressive size fallback used
// for "file too large". Only the first chunk is scanned - SOF markers appear
// before the entropy-coded scan data, which starts well within this window
// for realistic Telegram photos/thumbnails.
#define JPEG_HEADER_SCAN_BYTES 65536

static bool jpeg_file_is_progressive(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    uint8_t *buf = malloc(JPEG_HEADER_SCAN_BYTES);
    if (!buf) {
        fclose(f);
        return false;
    }

    size_t n = fread(buf, 1, JPEG_HEADER_SCAN_BYTES, f);
    fclose(f);

    bool is_progressive = false;
    if (n >= 4 && buf[0] == 0xFF && buf[1] == 0xD8) {
        size_t i = 2;
        while (i + 4 <= n) {
            if (buf[i] != 0xFF) {
                i++;
                continue;
            }
            uint8_t marker = buf[i + 1];
            // Markers with no payload (RSTn, SOI dup, TEM, padding 0xFF).
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9) || marker == 0xFF) {
                i += 2;
                continue;
            }
            uint16_t seg_len = (uint16_t) ((buf[i + 2] << 8) | buf[i + 3]);
            if (marker == 0xC0 || marker == 0xC1) {  // SOF0 / SOF1: baseline / extended sequential
                break;
            }
            if (marker == 0xC2 || marker == 0xC3) {  // SOF2 / SOF3: progressive / lossless
                is_progressive = true;
                break;
            }
            if (marker == 0xDA) {  // SOS: entropy-coded data follows, no SOF found before it
                break;
            }
            if (seg_len < 2) {
                break;  // malformed segment length, stop scanning
            }
            i += 2 + seg_len;
        }
    }

    free(buf);
    return is_progressive;
}

// ----------------------------------------------------------------------------
// Image download with progressive-size fallback
// ----------------------------------------------------------------------------

static esp_err_t make_unique_telegram_path(const char *ext, char *out, size_t out_len)
{
    time_t now = time(NULL);
    for (int suffix = 0; suffix < 100; suffix++) {
        if (suffix == 0) {
            snprintf(out, out_len, "%s/img_%lld.%s", TELEGRAM_DOWNLOAD_DIRECTORY, (long long) now,
                     ext);
        } else {
            snprintf(out, out_len, "%s/img_%lld_%d.%s", TELEGRAM_DOWNLOAD_DIRECTORY,
                     (long long) now, suffix, ext);
        }
        struct stat st;
        if (stat(out, &st) != 0) {
            return ESP_OK;  // path is free
        }
    }
    ESP_LOGE(TAG, "Could not find a free filename under %s", TELEGRAM_DOWNLOAD_DIRECTORY);
    return ESP_FAIL;
}

// Downloads a Telegram "photo" (array of re-encoded resolutions, ascending
// size) trying the largest first. Falls back to the next smaller size when
// the current one is too large for getFile, fails to download, or turns out
// to be a progressive JPEG we can't decode - until one succeeds or the
// smallest size has also failed.
//
// out_thumb_file_id is filled with the smallest available size's file_id
// regardless of which size was actually saved - Telegram already hosts it,
// so it can be echoed straight back via sendPhoto as a lightweight "saved"
// confirmation without re-uploading anything.
static esp_err_t download_photo_with_fallback(cJSON *photo_array, char *out_path,
                                               size_t out_path_len, char *out_thumb_file_id,
                                               size_t out_thumb_file_id_len)
{
    if (out_thumb_file_id && out_thumb_file_id_len > 0) {
        out_thumb_file_id[0] = '\0';
        cJSON *smallest = cJSON_GetArrayItem(photo_array, 0);
        cJSON *smallest_id = smallest ? cJSON_GetObjectItem(smallest, "file_id") : NULL;
        if (smallest_id && cJSON_IsString(smallest_id)) {
            strncpy(out_thumb_file_id, smallest_id->valuestring, out_thumb_file_id_len - 1);
            out_thumb_file_id[out_thumb_file_id_len - 1] = '\0';
        }
    }

    int n = cJSON_GetArraySize(photo_array);
    for (int i = n - 1; i >= 0; i--) {
        cJSON *size_obj = cJSON_GetArrayItem(photo_array, i);
        cJSON *file_id_item = cJSON_GetObjectItem(size_obj, "file_id");
        if (!file_id_item || !cJSON_IsString(file_id_item)) {
            continue;
        }

        cJSON *fsize_item = cJSON_GetObjectItem(size_obj, "file_size");
        int approx_kb =
            (fsize_item && cJSON_IsNumber(fsize_item)) ? (int) (fsize_item->valuedouble / 1024) : -1;
        ESP_LOGI(TAG, "Trying Telegram photo size %d/%d (~%d KB)", i + 1, n, approx_kb);

        if (make_unique_telegram_path("jpg", out_path, out_path_len) != ESP_OK) {
            return ESP_FAIL;
        }

        if (telegram_download_file_id(file_id_item->valuestring, out_path) != ESP_OK) {
            ESP_LOGW(TAG, "Size %d/%d failed to download, falling back to smaller", i + 1, n);
            continue;
        }

        if (jpeg_file_is_progressive(out_path)) {
            ESP_LOGW(TAG,
                     "Size %d/%d is a progressive JPEG (unsupported by decoder), falling back to "
                     "smaller",
                     i + 1, n);
            unlink(out_path);
            continue;
        }

        return ESP_OK;
    }

    ESP_LOGE(TAG, "All %d photo size(s) failed (progressive fallback exhausted)", n);
    return ESP_FAIL;
}

// Documents (files sent "as file") are not re-encoded by Telegram, so they
// keep their original format/encoding and there is only a single size - no
// fallback ladder to walk, just one attempt.
static bool document_pick_extension(cJSON *document, const char **out_ext)
{
    cJSON *mime = cJSON_GetObjectItem(document, "mime_type");
    cJSON *fname = cJSON_GetObjectItem(document, "file_name");
    const char *ext = NULL;

    if (mime && cJSON_IsString(mime)) {
        const char *m = mime->valuestring;
        if (strcmp(m, "image/jpeg") == 0) {
            ext = "jpg";
        } else if (strcmp(m, "image/png") == 0) {
            ext = "png";
        } else if (strcmp(m, "image/bmp") == 0 || strcmp(m, "image/x-ms-bmp") == 0) {
            ext = "bmp";
        }
    }
    if (!ext && fname && cJSON_IsString(fname)) {
        const char *dot = strrchr(fname->valuestring, '.');
        if (dot) {
            if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
                ext = "jpg";
            } else if (strcasecmp(dot, ".png") == 0) {
                ext = "png";
            } else if (strcasecmp(dot, ".bmp") == 0) {
                ext = "bmp";
            } else if (strcasecmp(dot, ".epdgz") == 0) {
                ext = "epdgz";
            }
        }
    }

    *out_ext = ext;
    return ext != NULL;
}

// out_thumb_file_id is filled from the document's own "thumbnail" (or the
// older "thumb" field name), if Telegram provided one - documents have no
// smaller sizes of their own, so this is the only lightweight option for a
// photo-reply confirmation; left empty if unavailable (caller falls back to
// a plain text reply).
static esp_err_t download_document_image(cJSON *document, char *out_path, size_t out_path_len,
                                          char *out_thumb_file_id, size_t out_thumb_file_id_len)
{
    if (out_thumb_file_id && out_thumb_file_id_len > 0) {
        out_thumb_file_id[0] = '\0';
        cJSON *thumb = cJSON_GetObjectItem(document, "thumbnail");
        if (!thumb) {
            thumb = cJSON_GetObjectItem(document, "thumb");
        }
        cJSON *thumb_id = thumb ? cJSON_GetObjectItem(thumb, "file_id") : NULL;
        if (thumb_id && cJSON_IsString(thumb_id)) {
            strncpy(out_thumb_file_id, thumb_id->valuestring, out_thumb_file_id_len - 1);
            out_thumb_file_id[out_thumb_file_id_len - 1] = '\0';
        }
    }

    cJSON *file_id_item = cJSON_GetObjectItem(document, "file_id");
    if (!file_id_item || !cJSON_IsString(file_id_item)) {
        return ESP_FAIL;
    }

    const char *ext = NULL;
    if (!document_pick_extension(document, &ext)) {
        ESP_LOGW(TAG, "Document has an unsupported format, skipping");
        return ESP_FAIL;
    }

    if (make_unique_telegram_path(ext, out_path, out_path_len) != ESP_OK) {
        return ESP_FAIL;
    }

    if (telegram_download_file_id(file_id_item->valuestring, out_path) != ESP_OK) {
        return ESP_FAIL;
    }

    if (strcmp(ext, "jpg") == 0 && jpeg_file_is_progressive(out_path)) {
        ESP_LOGW(TAG, "Document JPEG is progressive (unsupported by decoder)");
        unlink(out_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

// Runs the downloaded image through the existing processing pipeline (same
// format handling as fetch_and_display_image_from_url in utils.c: EPDGZ/BMP
// are already display-ready, PNG/JPG go through image_processor_process) and
// shows it. The original file under TELEGRAM_DOWNLOAD_DIRECTORY is left in
// place for later rotation cycles.
//
// If `caption` is non-empty, it's overlaid as a caption bar on the displayed
// image (only supported for the PNG/JPG path - EPDGZ/BMP are already
// display-ready blobs with no RGB buffer to draw into).
static esp_err_t process_and_display_telegram_image(const char *path, const char *caption)
{
    image_format_t format = image_processor_detect_format(path);

    if (format == IMAGE_FORMAT_EPD_GZ || format == IMAGE_FORMAT_BMP) {
        return display_manager_show_image(path);
    }

    if (format != IMAGE_FORMAT_PNG && format != IMAGE_FORMAT_JPG) {
        ESP_LOGE(TAG, "Unsupported/undetected image format for %s", path);
        return ESP_FAIL;
    }

    if (format == IMAGE_FORMAT_PNG && image_processor_is_processed(path)) {
        if (caption && caption[0] != '\0') {
            image_processor_add_caption_to_file(path, caption);
        }
        return display_manager_show_image(path);
    }

    dither_algorithm_t algo = processing_settings_get_dithering_algorithm();
    esp_err_t err = image_processor_process(path, CURRENT_PNG_PATH, algo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to process Telegram image %s: %s", path, esp_err_to_name(err));
        return err;
    }

    if (caption && caption[0] != '\0') {
        image_processor_add_caption_to_file(CURRENT_PNG_PATH, caption);
    }

    esp_err_t show_err = display_manager_show_image(CURRENT_PNG_PATH);
    if (show_err == ESP_OK) {
        // CURRENT_PNG_PATH is a shared scratch file reused for every
        // processed image, so display_manager_show_image()'s own history
        // hook records the wrong identifier here - mark the real source
        // path instead, so fallback album rotation (trigger_image_rotation)
        // knows this specific Telegram image has already been shown.
        history_manager_mark_shown(path);
    }
    return show_err;
}

// Generates a small preview thumbnail sidecar next to an already-persisted
// (processed, display-ready) Telegram image, named "<basename>.jpg" - the
// same sidecar convention the Web UI upload path uses (a client-generated
// real JPEG there; here it's PNG-encoded bytes under a ".jpg" name, since
// the firmware has no JPEG encoder - browsers render by sniffing content,
// not by trusting the extension, so this displays fine). Using ".jpg"
// unconditionally (not swapping to match the source's own extension) is
// deliberate: the source is always finalized to ".png" by
// finalize_telegram_image() before this runs, and a same-named ".png"
// thumbnail would silently overwrite the full-size image it's a thumbnail
// of. Best-effort: logs and returns on failure, never treated as fatal by
// the caller - a missing thumbnail just falls back to the icon+filename
// placeholder in the gallery.
static void generate_telegram_thumbnail(const char *image_path)
{
    image_format_t format = image_processor_detect_format(image_path);
    if (format != IMAGE_FORMAT_JPG && format != IMAGE_FORMAT_PNG) {
        return;
    }

    const char *source_path = image_path;
    if (format == IMAGE_FORMAT_JPG || !image_processor_is_processed(image_path)) {
        // Shouldn't normally happen anymore (finalize_telegram_image() below
        // converts to a processed PNG first) - kept as a defensive fallback
        // for whatever it didn't convert (e.g. processing failed upstream).
        dither_algorithm_t algo = processing_settings_get_dithering_algorithm();
        esp_err_t err = image_processor_process(image_path, TELEGRAM_THUMB_SCRATCH_PATH, algo);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Thumbnail: failed to process %s: %s", image_path,
                     esp_err_to_name(err));
            return;
        }
        source_path = TELEGRAM_THUMB_SCRATCH_PATH;
    }

    char thumb_path[320];
    strncpy(thumb_path, image_path, sizeof(thumb_path) - 1);
    thumb_path[sizeof(thumb_path) - 1] = '\0';
    char *ext = strrchr(thumb_path, '.');
    if (!ext || (size_t) (ext - thumb_path) + 4 >= sizeof(thumb_path)) {
        return;
    }
    strcpy(ext, ".jpg");

    esp_err_t err = image_processor_make_thumbnail(source_path, TELEGRAM_THUMBNAIL_MAX_DIMENSION,
                                                   thumb_path);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Thumbnail: failed to generate for %s: %s", image_path,
                 esp_err_to_name(err));
    }
}

// Converts a downloaded Telegram image to a permanent, properly persisted
// PNG in place (same directory/basename, extension -> .png) if it isn't
// already one, then generates its thumbnail sidecar. Raw downloaded JPEGs
// (the normal case - Telegram "photo" sends are always JPEG) otherwise stay
// invisible to the rest of the system: the Web UI gallery
// (album_images_handler) and fallback album rotation (rotate_sequential /
// rotate_random in display_manager.c) both only recognize .bmp/.png/.epdgz,
// never a bare .jpg. This is what a manual Web UI upload already gets for
// free (client-side conversion before upload); Telegram downloads need it
// done here instead, since there's no client-side step in that path.
//
// Must be called AFTER any orientation-mismatch/pairing decision that needs
// the image's original aspect ratio: a "processed" PNG is always padded to
// the panel's fixed display resolution, which no longer reflects the source
// photo's own portrait/landscape shape.
//
// `path` is updated in place if the file was converted. Best-effort: on
// processing failure, leaves `path` untouched (falls back to the original
// raw file, same as before this existed) and only logs a warning.
static void finalize_telegram_image(char *path, size_t path_len)
{
    image_format_t format = image_processor_detect_format(path);
    bool needs_conversion =
        (format == IMAGE_FORMAT_JPG || (format == IMAGE_FORMAT_PNG && !image_processor_is_processed(path)));

    if (needs_conversion) {
        char png_path[320];
        strncpy(png_path, path, sizeof(png_path) - 1);
        png_path[sizeof(png_path) - 1] = '\0';
        char *ext = strrchr(png_path, '.');
        if (ext && (size_t) (ext - png_path) + 4 < sizeof(png_path)) {
            strcpy(ext, ".png");
            dither_algorithm_t algo = processing_settings_get_dithering_algorithm();
            esp_err_t err = image_processor_process(path, png_path, algo);
            if (err == ESP_OK) {
                unlink(path);
                strncpy(path, png_path, path_len - 1);
                path[path_len - 1] = '\0';
            } else {
                ESP_LOGW(TAG, "Failed to persist %s as PNG, keeping original: %s", path,
                         esp_err_to_name(err));
            }
        }
    }

    generate_telegram_thumbnail(path);
}

// Reads an entire file into a heap_caps (SPIRAM) buffer.
static esp_err_t read_whole_file(const char *path, uint8_t **out_data, long *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return ESP_FAIL;
    }
    uint8_t *buf = (uint8_t *) heap_caps_malloc((size_t) size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t read_bytes = fread(buf, 1, (size_t) size, f);
    fclose(f);
    if (read_bytes != (size_t) size) {
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    *out_data = buf;
    *out_size = size;
    return ESP_OK;
}

// Uploads a local file as a brand-new Telegram photo (multipart/form-data
// POST to sendPhoto) - unlike telegram_bot_send_photo_reply() above, which
// re-references an existing Telegram-hosted file_id, this actually uploads
// bytes Telegram has never seen (e.g. a locally generated thumbnail). Kept
// to small files (thumbnails) - buffers the whole multipart body in SPIRAM
// rather than streaming, which keeps this simple and is fine at that size.
static esp_err_t telegram_bot_send_photo_file(const char *file_path, const char *caption)
{
    if (!config_manager_telegram_is_configured() || !file_path) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *file_data = NULL;
    long file_size = 0;
    if (read_whole_file(file_path, &file_data, &file_size) != ESP_OK) {
        ESP_LOGW(TAG, "send_photo_file: failed to read %s", file_path);
        return ESP_FAIL;
    }

    const char *boundary = "----ESP32PhotoFrameBoundary";
    const char *filename = strrchr(file_path, '/');
    filename = filename ? filename + 1 : file_path;
    const char *chat_id = config_manager_get_telegram_chat_id();

    char part1[512];
    int part1_len =
        snprintf(part1, sizeof(part1),
                 "--%s\r\n"
                 "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n%s\r\n"
                 "--%s\r\n"
                 "Content-Disposition: form-data; name=\"caption\"\r\n\r\n%.900s\r\n"
                 "--%s\r\n"
                 "Content-Disposition: form-data; name=\"photo\"; filename=\"%.100s\"\r\n"
                 "Content-Type: image/jpeg\r\n\r\n",
                 boundary, chat_id ? chat_id : "", boundary, caption ? caption : "", boundary,
                 filename);
    if (part1_len < 0 || (size_t) part1_len >= sizeof(part1)) {
        heap_caps_free(file_data);
        return ESP_FAIL;
    }

    char part2[64];
    int part2_len = snprintf(part2, sizeof(part2), "\r\n--%s--\r\n", boundary);

    size_t total_len = (size_t) part1_len + (size_t) file_size + (size_t) part2_len;
    uint8_t *multipart_body = (uint8_t *) heap_caps_malloc(total_len, MALLOC_CAP_SPIRAM);
    if (!multipart_body) {
        heap_caps_free(file_data);
        return ESP_ERR_NO_MEM;
    }
    memcpy(multipart_body, part1, (size_t) part1_len);
    memcpy(multipart_body + part1_len, file_data, (size_t) file_size);
    memcpy(multipart_body + part1_len + file_size, part2, (size_t) part2_len);
    heap_caps_free(file_data);

    char url[256];
    build_api_url("sendPhoto", url, sizeof(url));
    char content_type[64];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);

    esp_err_t result = ESP_FAIL;
    for (int attempt = 1; attempt <= TELEGRAM_HTTP_RETRY_COUNT; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "Retrying sendPhoto upload (%d/%d) after %d ms...", attempt,
                     TELEGRAM_HTTP_RETRY_COUNT, TELEGRAM_HTTP_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(TELEGRAM_HTTP_RETRY_DELAY_MS));
        }

        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = TELEGRAM_HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            continue;
        }

        esp_http_client_set_header(client, "Content-Type", content_type);
        esp_http_client_set_post_field(client, (const char *) multipart_body, (int) total_len);

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK || status != 200) {
            ESP_LOGW(TAG, "sendPhoto upload failed (err=%s, status=%d)", esp_err_to_name(err),
                     status);
            continue;
        }
        result = ESP_OK;
        break;
    }

    heap_caps_free(multipart_body);
    return result;
}

// Notifies Telegram about an image that was displayed via fallback album
// rotation (i.e. NOT from a Telegram push) - see
// config_manager_get_telegram_rotation_notify_enabled(). Sends the image's
// thumbnail sidecar if one exists (much smaller/faster to upload than the
// full display-resolution image), falling back to the full image otherwise.
esp_err_t telegram_bot_notify_fallback_image(const char *image_path)
{
    if (!image_path || image_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char thumb_path[320];
    strncpy(thumb_path, image_path, sizeof(thumb_path) - 1);
    thumb_path[sizeof(thumb_path) - 1] = '\0';
    char *ext = strrchr(thumb_path, '.');
    const char *send_path = image_path;
    if (ext && (size_t) (ext - thumb_path) + 4 < sizeof(thumb_path)) {
        strcpy(ext, ".jpg");
        struct stat st;
        if (strcmp(thumb_path, image_path) != 0 && stat(thumb_path, &st) == 0) {
            send_path = thumb_path;
        }
    }

    const char *fname = strrchr(image_path, '/');
    fname = fname ? fname + 1 : image_path;
    char caption[192];
    snprintf(caption, sizeof(caption), "[i] Auto-rotate (no new Telegram image): %.100s", fname);

    return telegram_bot_send_photo_file(send_path, caption);
}

// Reads just enough of a file to determine its pixel dimensions (bounded
// read - reuses the same header-scan window as the progressive-JPEG check).
// Whether the frame is currently mounted in portrait as *actually rendered*.
// display_rotation_deg (not display_orientation, which is only ever sent as
// an HTTP hint to external URL servers and has no on-device rendering
// effect) is what display_manager applies at the final blit stage via
// Paint_NewImage() - a 90/270 correction visually swaps the aspect ratio the
// viewer sees, regardless of the native panel's fixed pixel layout. Using it
// here keeps the pairing decision consistent with what the user actually
// sees, including the case where they only rotated the physical stand.
static bool wants_portrait_frame_now(void)
{
    int rot = config_manager_get_display_rotation_deg() % 360;
    if (rot < 0) {
        rot += 360;
    }
    return (rot == 90 || rot == 270);
}

// Composes two source images into a real PNG file under
// TELEGRAM_DOWNLOAD_DIRECTORY (so the result becomes a normal persisted,
// already-processed image - showable and available for storage-mode
// rotation like any other) and returns its path.
static esp_err_t compose_pair_and_save(const char *path_a, const char *caption_a,
                                       const char *path_b, const char *caption_b, char *out_path,
                                       size_t out_path_len)
{
    uint8_t *buf_a = NULL, *buf_b = NULL;
    long size_a = 0, size_b = 0;

    esp_err_t err = read_whole_file(path_a, &buf_a, &size_a);
    if (err != ESP_OK) {
        return err;
    }
    err = read_whole_file(path_b, &buf_b, &size_b);
    if (err != ESP_OK) {
        heap_caps_free(buf_a);
        return err;
    }

    image_format_t format_a = image_processor_detect_format(path_a);
    image_format_t format_b = image_processor_detect_format(path_b);
    dither_algorithm_t algo = processing_settings_get_dithering_algorithm();

    image_process_rgb_result_t result;
    // Older (first-arrived) image goes in the first slot, newer in the
    // second, matching arrival order.
    err = image_processor_compose_pair_to_rgb(buf_a, (size_t) size_a, format_a, buf_b,
                                              (size_t) size_b, format_b,
                                              wants_portrait_frame_now(), algo, &result);
    heap_caps_free(buf_a);
    heap_caps_free(buf_b);
    if (err != ESP_OK) {
        return err;
    }

    const char *overlay = (caption_b && caption_b[0])   ? caption_b
                         : (caption_a && caption_a[0]) ? caption_a
                                                        : NULL;
    if (overlay) {
        image_processor_draw_caption(result.rgb_data, result.width, result.height, overlay);
    }

    if (make_unique_telegram_path("png", out_path, out_path_len) != ESP_OK) {
        heap_caps_free(result.rgb_data);
        return ESP_FAIL;
    }
    err = image_processor_write_rgb_to_png(result.rgb_data, result.width, result.height, out_path);
    heap_caps_free(result.rgb_data);
    return err;
}

// Returns true and fills *out_percent with a valid 0-100 reading only when a
// battery is actually present and measurable. board_hal_get_battery_percent()
// returns -1 for "unknown" - notably also on pure-USB power with no battery
// installed, which must never be treated as "critically low".
static bool get_valid_battery_percent(int *out_percent)
{
    if (!board_hal_is_battery_connected()) {
        return false;
    }
    int percent = board_hal_get_battery_percent();
    if (percent < 0) {
        return false;
    }
    *out_percent = percent;
    return true;
}

// Sends a one-time-per-episode low-battery warning via Telegram, even if
// this poll had no new updates at all. Debounced via a persisted flag so it
// fires once per discharge, not on every wake while low; clears once the
// battery recovers past a small hysteresis margin.
#define TELEGRAM_LOW_BATTERY_THRESHOLD 20
#define TELEGRAM_LOW_BATTERY_CLEAR_THRESHOLD 25

static void check_and_warn_low_battery(void)
{
    int percent;
    if (!get_valid_battery_percent(&percent)) {
        return;
    }
    if (percent < TELEGRAM_LOW_BATTERY_THRESHOLD) {
        if (!config_manager_get_telegram_low_battery_warned()) {
            char msg[128];
            snprintf(msg, sizeof(msg), "[!] Low battery warning: Only %d%% remaining. Please charge soon.",
                     percent);
            telegram_bot_send_message(msg);
            config_manager_set_telegram_low_battery_warned(true);
        }
    } else if (percent >= TELEGRAM_LOW_BATTERY_CLEAR_THRESHOLD) {
        config_manager_set_telegram_low_battery_warned(false);
    }
}

// ----------------------------------------------------------------------------
// Update parsing helpers
// ----------------------------------------------------------------------------

static cJSON *get_message(cJSON *update_item)
{
    return cJSON_GetObjectItem(update_item, "message");
}

static bool message_from_allowed_chat(cJSON *message, const char *allowed_chat_id)
{
    if (!message || !allowed_chat_id || allowed_chat_id[0] == '\0') {
        return false;
    }
    cJSON *chat = cJSON_GetObjectItem(message, "chat");
    if (!chat) {
        return false;
    }
    cJSON *id_item = cJSON_GetObjectItem(chat, "id");
    if (!id_item || !cJSON_IsNumber(id_item)) {
        return false;
    }

    char id_str[24];
    snprintf(id_str, sizeof(id_str), "%lld", (long long) id_item->valuedouble);
    return strcmp(id_str, allowed_chat_id) == 0;
}

// Plain text messages carry the command in "text"; a photo/document sent with
// a caption (e.g. a photo captioned "/status") carries it in "caption"
// instead - Telegram never sets both on the same message, so text wins when
// present and caption is the fallback. Both the emergency reset scan and the
// command queue go through this, so a captioned "/telegram_reset" is caught
// too.
static const char *get_text(cJSON *message)
{
    cJSON *text_item = cJSON_GetObjectItem(message, "text");
    if (text_item && cJSON_IsString(text_item)) {
        return text_item->valuestring;
    }
    cJSON *caption_item = cJSON_GetObjectItem(message, "caption");
    if (caption_item && cJSON_IsString(caption_item)) {
        return caption_item->valuestring;
    }
    return NULL;
}

static void queue_command(const char *text)
{
    if (s_pending_command_count >= TELEGRAM_MAX_PENDING_COMMANDS) {
        ESP_LOGW(TAG, "Pending command queue full, dropping: %s", text);
        return;
    }
    strncpy(s_pending_commands[s_pending_command_count], text, TELEGRAM_COMMAND_MAX_LEN - 1);
    s_pending_commands[s_pending_command_count][TELEGRAM_COMMAND_MAX_LEN - 1] = '\0';
    s_pending_command_count++;
    ESP_LOGI(TAG, "Queued Telegram command: %s", text);
}

// Forward declaration - defined in the "Command execution" section below
// (shared with /status) but needed here for the optional wake-up ping.
static void build_status_message(const char *title, char *out, size_t out_len);

static void send_wake_notification_if_enabled(void)
{
    if (!config_manager_get_telegram_wake_notify_enabled()) {
        return;
    }
    char wake_msg[900];
    build_status_message("PhotoFrame awake", wake_msg, sizeof(wake_msg));
    telegram_bot_send_message(wake_msg);
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

esp_err_t telegram_bot_poll(telegram_poll_result_t *out_result)
{
    if (!config_manager_telegram_is_configured()) {
        if (out_result) {
            *out_result = TELEGRAM_POLL_NOT_CONFIGURED;
        }
        return ESP_ERR_INVALID_STATE;
    }

    // Ensure the download directory exists (also makes it show up as a
    // regular album under IMAGE_DIRECTORY for local-storage rotation).
    mkdir(TELEGRAM_DOWNLOAD_DIRECTORY, 0775);

    int64_t offset = config_manager_get_telegram_last_update_id() + 1;
    char base_url[256];
    build_api_url("getUpdates", base_url, sizeof(base_url));
    char url[350];
    snprintf(url, sizeof(url), "%s?offset=%lld&timeout=%d&limit=%d", base_url, (long long) offset,
             TELEGRAM_POLL_TIMEOUT_SEC, TELEGRAM_MAX_UPDATES_PER_POLL);

    char *body = NULL;
    esp_err_t err = telegram_http_get(
        url, TELEGRAM_HTTP_TIMEOUT_MS + TELEGRAM_POLL_TIMEOUT_SEC * 1000, &body, NULL);
    if (err != ESP_OK || !body) {
        ESP_LOGE(TAG, "getUpdates failed: %s", esp_err_to_name(err));
        free(body);
        if (out_result) {
            *out_result = TELEGRAM_POLL_ERROR;
        }
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGE(TAG, "getUpdates: failed to parse JSON response");
        if (out_result) {
            *out_result = TELEGRAM_POLL_ERROR;
        }
        return ESP_FAIL;
    }

    cJSON *ok_item = cJSON_GetObjectItem(root, "ok");
    cJSON *result_arr = cJSON_GetObjectItem(root, "result");
    if (!ok_item || !cJSON_IsTrue(ok_item) || !result_arr || !cJSON_IsArray(result_arr)) {
        cJSON *desc = cJSON_GetObjectItem(root, "description");
        ESP_LOGE(TAG, "getUpdates returned an error: %s",
                 (desc && cJSON_IsString(desc)) ? desc->valuestring : "unknown");
        cJSON_Delete(root);
        if (out_result) {
            *out_result = TELEGRAM_POLL_ERROR;
        }
        return ESP_FAIL;
    }

    // Runs every poll regardless of whether there are new updates, so a low
    // battery is reported even on an otherwise-quiet wake.
    check_and_warn_low_battery();

    int count = cJSON_GetArraySize(result_arr);
    if (count == 0) {
        ESP_LOGI(TAG, "No new Telegram updates");
        cJSON_Delete(root);
        send_wake_notification_if_enabled();
        if (out_result) {
            *out_result = TELEGRAM_POLL_OK_NO_IMAGE;
        }
        return ESP_OK;
    }

    const char *allowed_chat_id = config_manager_get_telegram_chat_id();
    int64_t max_update_id = offset - 1;

    // ---- First pass: highest update_id in the batch + emergency reset scan ----
    bool reset_found = false;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, result_arr)
    {
        cJSON *uid = cJSON_GetObjectItem(item, "update_id");
        if (uid && cJSON_IsNumber(uid)) {
            int64_t id = (int64_t) uid->valuedouble;
            if (id > max_update_id) {
                max_update_id = id;
            }
        }

        cJSON *message = get_message(item);
        if (!message_from_allowed_chat(message, allowed_chat_id)) {
            continue;
        }
        const char *text = get_text(message);
        if (text && strncmp(text, TELEGRAM_RESET_COMMAND, strlen(TELEGRAM_RESET_COMMAND)) == 0) {
            reset_found = true;
        }
    }

    if (reset_found) {
        ESP_LOGW(TAG, "/telegram_reset received - discarding %d update(s), going to sleep", count);
        config_manager_set_telegram_last_update_id(max_update_id);
        cJSON_Delete(root);
        telegram_bot_send_message("[OK] Reset executed, queue cleared.");
        if (out_result) {
            *out_result = TELEGRAM_POLL_RESET;
        }
        return ESP_OK;
    }

    // Optional wake-up ping, sent every poll (even with no new updates - see
    // the count==0 branch above) once the batch is confirmed not to be an
    // emergency reset - kept out of the reset path so a flooded queue still
    // resets as fast as possible.
    send_wake_notification_if_enabled();

    // ---- Second pass: download images, pair mismatched orientations inline
    // (in arrival order, so "newest ready wins" stays consistent whether the
    // winner is a normal image or a freshly-composed pair), queue commands ----
    //
    // These two tracking structs are heap-allocated, not stack locals: the
    // main task stack is only CONFIG_ESP_MAIN_TASK_STACK_SIZE bytes (6144 on
    // this project's boards) and the arrays below (320-byte path fields) add
    // up to well over that on their own - a stack array here would silently
    // overflow the task stack and crash.
    typedef struct {
        char path[320];
        char caption[TELEGRAM_CAPTION_MAX_LEN];
        char thumb_file_id[TELEGRAM_FILE_ID_MAX_LEN];
        char filename[64];
        int64_t message_id;
    } telegram_saved_image_t;

#define TELEGRAM_MAX_TRACKED_IMAGES 8
    telegram_saved_image_t *saved_images = (telegram_saved_image_t *) heap_caps_calloc(
        TELEGRAM_MAX_TRACKED_IMAGES, sizeof(telegram_saved_image_t), MALLOC_CAP_SPIRAM);
    int saved_image_count = 0;

    typedef struct {
        char path_a[320];
        char path_b[320];
        char composed_path[320];
        bool ok;
    } telegram_pair_result_t;
#define TELEGRAM_MAX_PAIR_RESULTS (TELEGRAM_MAX_TRACKED_IMAGES / 2 + 1)
    telegram_pair_result_t *pair_results = (telegram_pair_result_t *) heap_caps_calloc(
        TELEGRAM_MAX_PAIR_RESULTS, sizeof(telegram_pair_result_t), MALLOC_CAP_SPIRAM);
    int pair_result_count = 0;

    if (!saved_images || !pair_results) {
        ESP_LOGE(TAG, "Failed to allocate Telegram poll tracking buffers");
        heap_caps_free(saved_images);
        heap_caps_free(pair_results);
        cJSON_Delete(root);
        if (out_result) {
            *out_result = TELEGRAM_POLL_ERROR;
        }
        return ESP_ERR_NO_MEM;
    }

    char display_path[320] = {0};
    bool have_display_candidate = false;
    bool combined = false;
    bool image_attempt_failed = false;

    bool pairing_enabled = config_manager_get_telegram_pairing_enabled();
    bool wants_portrait = wants_portrait_frame_now();

    cJSON_ArrayForEach(item, result_arr)
    {
        cJSON *message = get_message(item);
        if (!message_from_allowed_chat(message, allowed_chat_id)) {
            ESP_LOGW(TAG, "Ignoring Telegram update from a disallowed chat");
            continue;
        }

        cJSON *photo = cJSON_GetObjectItem(message, "photo");
        cJSON *document = cJSON_GetObjectItem(message, "document");
        char downloaded_path[320];
        char thumb_file_id[TELEGRAM_FILE_ID_MAX_LEN];
        bool got_image = false;

        if (photo && cJSON_IsArray(photo) && cJSON_GetArraySize(photo) > 0) {
            got_image = (download_photo_with_fallback(photo, downloaded_path,
                                                       sizeof(downloaded_path), thumb_file_id,
                                                       sizeof(thumb_file_id)) == ESP_OK);
            if (!got_image) {
                image_attempt_failed = true;
            }
        } else if (document && cJSON_IsObject(document)) {
            got_image = (download_document_image(document, downloaded_path,
                                                  sizeof(downloaded_path), thumb_file_id,
                                                  sizeof(thumb_file_id)) == ESP_OK);
            if (!got_image) {
                image_attempt_failed = true;
            }
        }

        const char *text = get_text(message);
        // Only a genuine caption becomes a display overlay - not a "/"
        // command that happens to be attached to the same photo.
        const char *image_caption = (text && text[0] != '/') ? text : NULL;

        if (got_image) {
            ESP_LOGI(TAG, "Saved Telegram image: %s", downloaded_path);

            // Orientation mismatch must be checked against the ORIGINAL
            // image's own aspect ratio, before finalize_telegram_image()
            // below pads it to the panel's fixed display resolution.
            bool mismatch = false;
            if (pairing_enabled) {
                image_format_t fmt = image_processor_detect_format(downloaded_path);
                if (fmt == IMAGE_FORMAT_PNG || fmt == IMAGE_FORMAT_JPG) {
                    int w = 0, h = 0;
                    if (image_processor_peek_file_dimensions(downloaded_path, fmt, &w, &h) ==
                            ESP_OK &&
                        w > 0 && h > 0) {
                        mismatch = ((h > w) != wants_portrait);
                    }
                }
            }

            // Persist as a proper processed PNG (+ thumbnail sidecar) so
            // this image is visible to the Web UI gallery and fallback
            // album rotation, same as any other album image - see
            // finalize_telegram_image() for why a raw download isn't.
            finalize_telegram_image(downloaded_path, sizeof(downloaded_path));

            if (saved_image_count < TELEGRAM_MAX_TRACKED_IMAGES) {
                telegram_saved_image_t *entry = &saved_images[saved_image_count++];
                strncpy(entry->path, downloaded_path, sizeof(entry->path) - 1);
                entry->path[sizeof(entry->path) - 1] = '\0';
                strncpy(entry->caption, image_caption ? image_caption : "",
                        sizeof(entry->caption) - 1);
                entry->caption[sizeof(entry->caption) - 1] = '\0';
                strncpy(entry->thumb_file_id, thumb_file_id, sizeof(entry->thumb_file_id) - 1);
                entry->thumb_file_id[sizeof(entry->thumb_file_id) - 1] = '\0';
                const char *fname = strrchr(downloaded_path, '/');
                fname = fname ? fname + 1 : downloaded_path;
                strncpy(entry->filename, fname, sizeof(entry->filename) - 1);
                entry->filename[sizeof(entry->filename) - 1] = '\0';
                cJSON *mid = cJSON_GetObjectItem(message, "message_id");
                entry->message_id = (mid && cJSON_IsNumber(mid)) ? (int64_t) mid->valuedouble : 0;
            } else {
                ESP_LOGW(TAG, "Too many images in this batch, skipping reply confirmation for %s",
                         downloaded_path);
            }

            if (!mismatch) {
                // Orientation already matches the frame (or pairing doesn't
                // apply to this format) - shows normally, same as before.
                strncpy(display_path, downloaded_path, sizeof(display_path) - 1);
                display_path[sizeof(display_path) - 1] = '\0';
                have_display_candidate = true;
                combined = false;
            } else {
                bool paired = false;
                if (config_manager_get_telegram_pending_image_count() > 0 &&
                    pair_result_count < TELEGRAM_MAX_PAIR_RESULTS) {
                    char pending_path[320], pending_cap[TELEGRAM_CAPTION_MAX_LEN];
                    config_manager_get_telegram_pending_image_at(
                        0, pending_path, sizeof(pending_path), pending_cap, sizeof(pending_cap));

                    struct stat st;
                    if (stat(pending_path, &st) != 0) {
                        // Vanished (e.g. MemFS wiped by a deep-sleep reboot) -
                        // drop it; current image becomes the new pending entry
                        // below.
                        ESP_LOGW(TAG, "Pending pair image %s vanished, dropping", pending_path);
                        config_manager_remove_telegram_pending_image_at(0);
                    } else {
                        telegram_pair_result_t *pr = &pair_results[pair_result_count++];
                        strncpy(pr->path_a, pending_path, sizeof(pr->path_a) - 1);
                        pr->path_a[sizeof(pr->path_a) - 1] = '\0';
                        strncpy(pr->path_b, downloaded_path, sizeof(pr->path_b) - 1);
                        pr->path_b[sizeof(pr->path_b) - 1] = '\0';

                        esp_err_t compose_err = compose_pair_and_save(
                            pending_path, pending_cap, downloaded_path, image_caption,
                            pr->composed_path, sizeof(pr->composed_path));
                        pr->ok = (compose_err == ESP_OK);
                        config_manager_remove_telegram_pending_image_at(0);
                        paired = true;

                        if (pr->ok) {
                            generate_telegram_thumbnail(pr->composed_path);
                            ESP_LOGI(TAG, "Composed and saved paired image: %s", pr->composed_path);
                            strncpy(display_path, pr->composed_path, sizeof(display_path) - 1);
                            display_path[sizeof(display_path) - 1] = '\0';
                            have_display_candidate = true;
                            combined = true;
                        } else {
                            ESP_LOGE(TAG, "Failed to compose pair (%s + %s): %s", pending_path,
                                     downloaded_path, esp_err_to_name(compose_err));
                        }
                    }
                }
                if (!paired) {
                    config_manager_add_telegram_pending_image(downloaded_path,
                                                              image_caption ? image_caption : "");
                }
            }
        }

        if (text && text[0] == '/') {
            queue_command(text);
        }
    }

    cJSON_Delete(root);

    bool displayed = false;
    esp_err_t disp_err = ESP_OK;
    if (have_display_candidate) {
        // A composed pair already has its caption baked in; a normal image
        // still needs its own caption applied at display time.
        const char *caption_for_display = NULL;
        if (!combined) {
            for (int i = saved_image_count - 1; i >= 0; i--) {
                if (strcmp(saved_images[i].path, display_path) == 0) {
                    caption_for_display = saved_images[i].caption[0] ? saved_images[i].caption : NULL;
                    break;
                }
            }
        }
        disp_err = process_and_display_telegram_image(display_path, caption_for_display);
        displayed = (disp_err == ESP_OK);
    } else if (image_attempt_failed) {
        telegram_bot_send_message(
            "[ERROR] Telegram image could not be loaded\n"
            "(too large, unsupported format, or only available as a progressive JPEG). "
            "Please send a smaller image or send it as a file.");
    }

    // Per-image "saved" confirmations, threaded as a reply to the original
    // message and (where Telegram gave us a thumbnail file_id) attaching the
    // smallest available photo size - already hosted by Telegram, so no
    // re-upload is needed.
    for (int i = 0; i < saved_image_count; i++) {
        telegram_saved_image_t *entry = &saved_images[i];
        char caption_text[192];

        int pair_index = -1;
        for (int j = 0; j < pair_result_count; j++) {
            if (strcmp(pair_results[j].path_a, entry->path) == 0 ||
                strcmp(pair_results[j].path_b, entry->path) == 0) {
                pair_index = j;
                break;
            }
        }

        bool still_pending = false;
        int pending_count = config_manager_get_telegram_pending_image_count();
        for (int j = 0; j < pending_count && !still_pending; j++) {
            char p[320];
            if (config_manager_get_telegram_pending_image_at(j, p, sizeof(p), NULL, 0) &&
                strcmp(p, entry->path) == 0) {
                still_pending = true;
            }
        }

        bool this_is_shown = have_display_candidate && strcmp(entry->path, display_path) == 0;
        // entry->path is always one of the two SOURCE images, never the
        // synthesized composed_path itself - so this compares against the
        // pair's own composed_path, not entry->path/this_is_shown.
        bool this_pair_is_shown = (pair_index >= 0 && pair_results[pair_index].ok && combined &&
                                   have_display_candidate &&
                                   strcmp(pair_results[pair_index].composed_path, display_path) ==
                                       0);

        if (this_pair_is_shown) {
            snprintf(caption_text, sizeof(caption_text),
                     "[OK] Saved & displayed combined with the previous image\n%.80s",
                     entry->filename);
        } else if (pair_index >= 0 && pair_results[pair_index].ok) {
            snprintf(caption_text, sizeof(caption_text),
                     "[OK] Saved & combined in the album (not displayed)\n%.60s",
                     entry->filename);
        } else if (pair_index >= 0) {
            snprintf(caption_text, sizeof(caption_text),
                     "[!] Saved, combining failed\n%.80s", entry->filename);
        } else if (this_is_shown && displayed) {
            snprintf(caption_text, sizeof(caption_text), "[OK] Saved & displayed\n%.100s",
                     entry->filename);
        } else if (this_is_shown) {
            snprintf(caption_text, sizeof(caption_text),
                     "[!] Saved (%.80s)\nDisplay failed: %.30s", entry->filename,
                     esp_err_to_name(disp_err));
        } else if (still_pending) {
            snprintf(caption_text, sizeof(caption_text),
                     "[OK] Saved, waiting for a portrait/landscape partner image\n%.100s",
                     entry->filename);
        } else {
            snprintf(caption_text, sizeof(caption_text), "[OK] Saved (queued)\n%.100s",
                     entry->filename);
        }

        if (entry->thumb_file_id[0] != '\0') {
            telegram_bot_send_photo_reply(entry->thumb_file_id, caption_text, entry->message_id);
        } else {
            telegram_bot_send_message_reply(caption_text, entry->message_id);
        }
    }

    heap_caps_free(saved_images);
    heap_caps_free(pair_results);

    config_manager_set_telegram_last_update_id(max_update_id);

    if (out_result) {
        *out_result = displayed ? TELEGRAM_POLL_OK : TELEGRAM_POLL_OK_NO_IMAGE;
    }
    return ESP_OK;
}

// ----------------------------------------------------------------------------
// Command execution
// ----------------------------------------------------------------------------

static const char *reset_reason_string(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return "Power-On";
    case ESP_RST_SW:
        return "Software-Reset";
    case ESP_RST_PANIC:
        return "Exception/Panic";
    case ESP_RST_INT_WDT:
        return "Interrupt-Watchdog";
    case ESP_RST_TASK_WDT:
        return "Task-Watchdog";
    case ESP_RST_WDT:
        return "Watchdog";
    case ESP_RST_DEEPSLEEP:
        return "Deep-Sleep-Wake";
    case ESP_RST_BROWNOUT:
        return "Brownout";
    default:
        return "Unknown";
    }
}

static void format_battery(char *out, size_t out_len)
{
    int percent;
    if (!get_valid_battery_percent(&percent)) {
        snprintf(out, out_len, board_hal_is_usb_connected() ? "USB connected (no battery detected)"
                                                             : "unknown");
        return;
    }
    int mv = board_hal_get_battery_voltage();
    snprintf(out, out_len, "%d%% (%d mV)%s%s", percent, mv,
             board_hal_is_charging() ? ", charging" : "",
             board_hal_is_usb_connected() ? ", USB connected" : "");
}

static void format_free_storage(char *out, size_t out_len)
{
    storage_type_t type = storage_get_type();
    uint64_t total = 0, free_bytes = 0;
    bool have = false;

    if (type == STORAGE_TYPE_SDCARD) {
        uint64_t t = 0, f = 0;
        if (esp_vfs_fat_info(FS_MOUNT_POINT, &t, &f) == ESP_OK) {
            total = t;
            free_bytes = f;
            have = true;
        }
    } else if (type == STORAGE_TYPE_LITTLEFS) {
        size_t t = 0, u = 0;
        if (esp_littlefs_info(LITTLEFS_PARTITION_LABEL, &t, &u) == ESP_OK) {
            total = t;
            free_bytes = (t > u) ? (t - u) : 0;
            have = true;
        }
    }

    if (!have) {
        snprintf(out, out_len, "n/a");
        return;
    }
    int percent = (total > 0) ? (int) ((free_bytes * 100ULL) / total) : 0;
    snprintf(out, out_len, "%.1f/%.1f MB free (%d%%)", free_bytes / (1024.0 * 1024.0),
             total / (1024.0 * 1024.0), percent);
}

static void format_heap(char *out, size_t out_len)
{
    size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t total_bytes = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    int percent = (total_bytes > 0) ? (int) ((free_bytes * 100ULL) / total_bytes) : 0;
    snprintf(out, out_len, "%.1f/%.1f MB free (%d%%)", free_bytes / (1024.0 * 1024.0),
             total_bytes / (1024.0 * 1024.0), percent);
}

static void format_toggles(char *out, size_t out_len)
{
    snprintf(out, out_len,
             "[%c] Telegram pairing (portrait/landscape combine)\n"
             "[%c] Deep Sleep\n"
             "[%c] Auto-Rotate\n"
             "[%c] Wake notification\n"
             "[%c] Error overlay\n"
             "[%c] WiFi performance\n"
             "[%c] Rotation pairing (random mode only)\n"
             "[%c] Rotation notify (thumbnail on fallback display)",
             config_manager_get_telegram_pairing_enabled() ? 'x' : ' ',
             config_manager_get_deep_sleep_enabled() ? 'x' : ' ',
             config_manager_get_auto_rotate() ? 'x' : ' ',
             config_manager_get_telegram_wake_notify_enabled() ? 'x' : ' ',
             config_manager_get_error_overlay_enabled() ? 'x' : ' ',
             config_manager_get_wifi_performance_mode_enabled() ? 'x' : ' ',
             config_manager_get_rotation_pairing_enabled() ? 'x' : ' ',
             config_manager_get_telegram_rotation_notify_enabled() ? 'x' : ' ');
}

static void format_rotation_schedule(char *out, size_t out_len)
{
    if (!config_manager_get_auto_rotate()) {
        snprintf(out, out_len, "Auto-Rotate disabled");
        return;
    }
    int count = config_manager_get_cron_rule_count();
    if (count == 0) {
        snprintf(out, out_len, "no schedule configured");
        return;
    }
    size_t off = 0;
    out[0] = '\0';
    for (int i = 0; i < count && off < out_len; i++) {
        const char *rule = config_manager_get_cron_rule(i);
        if (!rule) {
            continue;
        }
        int n = snprintf(out + off, out_len - off, "%s%s", i ? ", " : "", rule);
        if (n < 0 || (size_t) n >= out_len - off) {
            break;
        }
        off += (size_t) n;
    }
}

// Shared by /status and the optional wake-up notification - `title` is the
// only thing that differs between the two use sites.
static void format_battery_estimate(char *out, size_t out_len)
{
    double days;
    if (!battery_history_estimate_days_remaining(&days)) {
        snprintf(out, out_len, "not enough history yet");
        return;
    }
    if (days <= 0) {
        snprintf(out, out_len, "already at or below %d%%", BATTERY_HISTORY_TARGET_PERCENT);
    } else {
        snprintf(out, out_len, "~%.1f days until %d%%", days, BATTERY_HISTORY_TARGET_PERCENT);
    }
}

static void build_status_message(const char *title, char *out, size_t out_len)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();

    char battery[64];
    format_battery(battery, sizeof(battery));

    char battery_estimate[64];
    format_battery_estimate(battery_estimate, sizeof(battery_estimate));

    char ip_str[16] = "n/a";
    wifi_manager_get_ip(ip_str, sizeof(ip_str));

    char storage[64];
    format_free_storage(storage, sizeof(storage));

    char heap[64];
    format_heap(heap, sizeof(heap));

    char schedule[160];
    format_rotation_schedule(schedule, sizeof(schedule));

    char toggles[320];
    format_toggles(toggles, sizeof(toggles));

    const char *ssid = config_manager_get_wifi_ssid();

    snprintf(out, out_len,
             "=== %s ===\n"
             "Firmware: %s (%s)\n"
             "Reset reason: %s\n"
             "\n"
             "Battery: %s\n"
             "Battery estimate: %s\n"
             "WiFi: %s (%s)\n"
             "\n"
             "Storage: %s\n"
             "Heap: %s\n"
             "\n"
             "Rotation schedule: %s\n"
             "\n"
             "Settings:\n"
             "%s",
             title, app_desc->version, BOARD_HAL_NAME, reset_reason_string(), battery,
             battery_estimate, ssid ? ssid : "n/a", ip_str, storage, heap, schedule, toggles);
}

// Executes one queued "/"-command and sends a sendMessage reply. Strips an
// optional "@BotName" suffix (Telegram appends it in group chats) and splits
// off any argument text after the command token (e.g. "/rotate_cron 0 */12 *").
static void execute_command(const char *raw_text)
{
    char full[TELEGRAM_COMMAND_MAX_LEN];
    strncpy(full, raw_text, sizeof(full) - 1);
    full[sizeof(full) - 1] = '\0';

    char *args = NULL;
    char *space = strpbrk(full, " \t\n@");
    if (space) {
        bool had_at = (*space == '@');
        *space = '\0';
        // Plain space: `space` itself is the delimiter before the args.
        // '@BotName' suffix: skip past the username to find the real
        // delimiter, if any, before the args.
        char *delim = had_at ? strpbrk(space + 1, " \t\n") : space;
        if (delim) {
            args = delim + 1;
            while (*args == ' ' || *args == '\t') {
                args++;
            }
            if (*args == '\0') {
                args = NULL;
            }
        }
    }
    const char *cmd = full;

    ESP_LOGI(TAG, "Executing Telegram command: %s%s%s", cmd, args ? " " : "", args ? args : "");

    if (strcmp(cmd, "/status") == 0) {
        char msg[900];
        build_status_message("PhotoFrame Status", msg, sizeof(msg));
        telegram_bot_send_message(msg);
    } else if (strcmp(cmd, "/clear") == 0) {
        esp_err_t err = display_manager_clear();
        telegram_bot_send_message(err == ESP_OK ? "[OK] Display cleared."
                                                 : "[ERROR] Failed to clear display.");
    } else if (strcmp(cmd, "/restart") == 0) {
        telegram_bot_send_message("[OK] Restarting...");
        vTaskDelay(pdMS_TO_TICKS(500));  // give the HTTP send a moment to flush
        esp_restart();
        // Does not return.
    } else if (strcmp(cmd, "/pairing") == 0) {
        bool enabled = !config_manager_get_telegram_pairing_enabled();
        config_manager_set_telegram_pairing_enabled(enabled);
        if (!enabled) {
            // Turning pairing off drops the tracking queue only - the files
            // themselves stay on storage, nothing is deleted.
            config_manager_clear_telegram_pending_images();
        }
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "[%c] Portrait/landscape combining\n"
                 "Current frame orientation: %s",
                 enabled ? 'x' : ' ',
                 wants_portrait_frame_now() ? "portrait" : "landscape");
        telegram_bot_send_message(msg);
    } else if (strcmp(cmd, "/rotate_cron") == 0) {
        if (!args) {
            telegram_bot_send_message(
                "[i] Usage: /rotate_cron <Minute Hour Weekday>\n"
                "Example: /rotate_cron 0 */12 *");
        } else {
            cron_rule_t tmp;
            if (!cron_parse(args, &tmp)) {
                char msg[192];
                snprintf(msg, sizeof(msg), "[ERROR] Invalid cron expression: %.100s", args);
                telegram_bot_send_message(msg);
            } else {
                const char *one[1] = {args};
                config_manager_set_cron_rules(one, 1);
                power_manager_reset_rotate_timer();
                char msg[192];
                snprintf(msg, sizeof(msg), "[OK] Rotation schedule set: %.100s", args);
                telegram_bot_send_message(msg);
            }
        }
    } else if (strcmp(cmd, "/deep_sleep") == 0) {
        if (args && strcasecmp(args, "on") == 0) {
            power_manager_set_deep_sleep_enabled(true);
            telegram_bot_send_message("[x] Deep Sleep enabled.");
        } else if (args && strcasecmp(args, "off") == 0) {
            power_manager_set_deep_sleep_enabled(false);
            telegram_bot_send_message("[ ] Deep Sleep disabled.");
        } else {
            telegram_bot_send_message("[i] Usage: /deep_sleep on|off");
        }
    } else if (strcmp(cmd, "/auto_rotate") == 0) {
        if (args && strcasecmp(args, "on") == 0) {
            config_manager_set_auto_rotate(true);
            power_manager_reset_rotate_timer();
            telegram_bot_send_message("[x] Auto-Rotate enabled.");
        } else if (args && strcasecmp(args, "off") == 0) {
            config_manager_set_auto_rotate(false);
            telegram_bot_send_message("[ ] Auto-Rotate disabled.");
        } else {
            telegram_bot_send_message("[i] Usage: /auto_rotate on|off");
        }
    } else if (strcmp(cmd, "/wake_notify") == 0) {
        if (args && strcasecmp(args, "on") == 0) {
            config_manager_set_telegram_wake_notify_enabled(true);
            telegram_bot_send_message("[x] Wake notification enabled.");
        } else if (args && strcasecmp(args, "off") == 0) {
            config_manager_set_telegram_wake_notify_enabled(false);
            telegram_bot_send_message("[ ] Wake notification disabled.");
        } else {
            telegram_bot_send_message("[i] Usage: /wake_notify on|off");
        }
    } else if (strcmp(cmd, "/error_overlay") == 0) {
        if (args && strcasecmp(args, "on") == 0) {
            config_manager_set_error_overlay_enabled(true);
            telegram_bot_send_message("[x] On-display error overlay enabled.");
        } else if (args && strcasecmp(args, "off") == 0) {
            config_manager_set_error_overlay_enabled(false);
            telegram_bot_send_message("[ ] On-display error overlay disabled.");
        } else {
            telegram_bot_send_message("[i] Usage: /error_overlay on|off");
        }
    } else if (strcmp(cmd, "/wifi_perf") == 0) {
        if (args && strcasecmp(args, "on") == 0) {
            config_manager_set_wifi_performance_mode_enabled(true);
            telegram_bot_send_message(
                "[x] WiFi performance mode enabled\n(automatically switches based on context).");
        } else if (args && strcasecmp(args, "off") == 0) {
            config_manager_set_wifi_performance_mode_enabled(false);
            telegram_bot_send_message(
                "[ ] WiFi performance mode disabled\n(always power-save, slower Web UI).");
        } else {
            telegram_bot_send_message("[i] Usage: /wifi_perf on|off");
        }
    } else if (strcmp(cmd, "/rotation_pairing") == 0) {
        if (args && strcasecmp(args, "on") == 0) {
            config_manager_set_rotation_pairing_enabled(true);
            telegram_bot_send_message(
                "[x] Auto-rotate orientation pairing enabled\n"
                "(random rotation mode only - has no effect in sequential mode).");
        } else if (args && strcasecmp(args, "off") == 0) {
            config_manager_set_rotation_pairing_enabled(false);
            telegram_bot_send_message("[ ] Auto-rotate orientation pairing disabled.");
        } else {
            telegram_bot_send_message("[i] Usage: /rotation_pairing on|off");
        }
    } else if (strcmp(cmd, "/rotation_notify") == 0) {
        if (args && strcasecmp(args, "on") == 0) {
            config_manager_set_telegram_rotation_notify_enabled(true);
            telegram_bot_send_message(
                "[x] Fallback-rotation notification enabled\n"
                "(sends a thumbnail whenever a wake shows an image that didn't come from "
                "Telegram).");
        } else if (args && strcasecmp(args, "off") == 0) {
            config_manager_set_telegram_rotation_notify_enabled(false);
            telegram_bot_send_message("[ ] Fallback-rotation notification disabled.");
        } else {
            telegram_bot_send_message("[i] Usage: /rotation_notify on|off");
        }
    } else if (strcmp(cmd, "/list_albums") == 0) {
        char **albums = NULL;
        int count = 0;
        if (album_manager_list_albums(&albums, &count) != ESP_OK) {
            telegram_bot_send_message("[ERROR] Could not load albums.");
        } else if (count == 0) {
            telegram_bot_send_message("[i] No albums exist.");
        } else {
            char msg[900];
            size_t off = 0;
            int n = snprintf(msg, sizeof(msg), "=== Albums (%d) ===\n", count);
            off = (n > 0) ? (size_t) n : 0;
            for (int i = 0; i < count && off < sizeof(msg); i++) {
                n = snprintf(msg + off, sizeof(msg) - off, "[%c] %.60s\n",
                             album_manager_is_album_enabled(albums[i]) ? 'x' : ' ', albums[i]);
                if (n < 0 || (size_t) n >= sizeof(msg) - off) {
                    break;
                }
                off += (size_t) n;
            }
            telegram_bot_send_message(msg);
        }
        album_manager_free_album_list(albums, count);
    } else if (strcmp(cmd, "/active_albums") == 0) {
        char **albums = NULL;
        int count = 0;
        if (album_manager_get_enabled_albums(&albums, &count) != ESP_OK || count == 0) {
            telegram_bot_send_message("[i] No active albums.");
        } else {
            char msg[900];
            size_t off = 0;
            int n = snprintf(msg, sizeof(msg), "=== Active albums (%d) ===\n", count);
            off = (n > 0) ? (size_t) n : 0;
            for (int i = 0; i < count && off < sizeof(msg); i++) {
                n = snprintf(msg + off, sizeof(msg) - off, "%.60s\n", albums[i]);
                if (n < 0 || (size_t) n >= sizeof(msg) - off) {
                    break;
                }
                off += (size_t) n;
            }
            telegram_bot_send_message(msg);
        }
        album_manager_free_album_list(albums, count);
    } else if (strcmp(cmd, "/enable_album") == 0) {
        if (!args) {
            telegram_bot_send_message("[i] Usage: /enable_album <albumname>");
        } else if (!album_manager_album_exists(args)) {
            char msg[192];
            snprintf(msg, sizeof(msg), "[ERROR] Album not found: %.100s", args);
            telegram_bot_send_message(msg);
        } else {
            esp_err_t aerr = album_manager_set_album_enabled(args, true);
            char msg[192];
            snprintf(msg, sizeof(msg),
                     aerr == ESP_OK ? "[x] Album enabled: %.100s"
                                    : "[ERROR] Could not enable album: %.100s",
                     args);
            telegram_bot_send_message(msg);
        }
    } else if (strcmp(cmd, "/clear_history") == 0) {
        history_manager_clear();
        // Also resets the sequential-rotation cursor so both rotation modes
        // start a fresh cycle, not just the random-mode history set.
        config_manager_set_last_index(-1);
        telegram_bot_send_message("[OK] Display history cleared.");
    } else if (strcmp(cmd, "/help") == 0) {
        telegram_bot_send_message(
            "=== Available commands ===\n"
            "\n"
            "Status:\n"
            "/status - Status, battery, WiFi, storage, settings\n"
            "\n"
            "Display:\n"
            "/clear - Clear the display\n"
            "/restart - Restart the photo frame\n"
            "/pairing - Toggle portrait/landscape combining\n"
            "\n"
            "Albums:\n"
            "/list_albums - List all albums\n"
            "/active_albums - List active albums\n"
            "/enable_album <albumname> - Enable an album\n"
            "/clear_history - Clear the display history (restart the cycle)\n"
            "\n"
            "Settings (each on|off, no argument = help):\n"
            "/rotate_cron <M H Weekday> - Set the rotation schedule\n"
            "/deep_sleep on|off\n"
            "/auto_rotate on|off\n"
            "/wake_notify on|off - Status ping on every wake-up\n"
            "/error_overlay on|off - On-display error notice\n"
            "/wifi_perf on|off - WiFi performance mode\n"
            "/rotation_pairing on|off - Combine mismatched-orientation images\n"
            "  during auto-rotation (random mode only, no effect in sequential mode)\n"
            "/rotation_notify on|off - Send a thumbnail when a wake displays an\n"
            "  image that didn't come from Telegram (i.e. fallback rotation)\n"
            "\n"
            "Emergency:\n"
            "/telegram_reset - Clear the queue immediately\n"
            "\n"
            "Images can be sent as a photo or as a file. A caption is overlaid on the "
            "image (unless it starts with \"/\").");
    } else {
        char msg[192];
        snprintf(msg, sizeof(msg), "[ERROR] Unknown command: %s\nSee /help for an overview.", cmd);
        telegram_bot_send_message(msg);
    }
}

void telegram_bot_run_pending_commands(void)
{
    for (int i = 0; i < s_pending_command_count; i++) {
        execute_command(s_pending_commands[i]);
    }
    s_pending_command_count = 0;
}
