#include "remote_gallery.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "remote_gallery";

#define HTTP_BUFFER_SIZE 4096

esp_err_t remote_gallery_download_image(void)
{
    ESP_LOGI(TAG, "Downloading from %s to %s", REMOTE_GALLERY_URL, TEMP_IMAGE_PATH);

    esp_http_client_config_t config = {
        .url = REMOTE_GALLERY_URL,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = HTTP_BUFFER_SIZE,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "Content-Length: %d", content_length);

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status code: %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    FILE *fp = fopen(TEMP_IMAGE_PATH, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", TEMP_IMAGE_PATH);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char *buffer = malloc(HTTP_BUFFER_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate download buffer");
        fclose(fp);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_bytes = 0;
    int read_len;
    while ((read_len = esp_http_client_read(client, buffer, HTTP_BUFFER_SIZE)) > 0) {
        size_t written = fwrite(buffer, 1, read_len, fp);
        if (written != read_len) {
            ESP_LOGE(TAG, "Failed to write to file");
            free(buffer);
            fclose(fp);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        total_bytes += read_len;
    }

    free(buffer);
    fclose(fp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len < 0) {
        ESP_LOGE(TAG, "HTTP read error");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Download complete: %d bytes written to %s", total_bytes, TEMP_IMAGE_PATH);
    return ESP_OK;
}
