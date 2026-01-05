#include "network_fetcher.h"

#include <stdio.h>

#include "esp_http_client.h"
#include "esp_log.h"
// #include "esp_tls.h"
#include "esp_crt_bundle.h"

static const char *TAG = "network_fetcher";

esp_err_t network_fetch_image(const char *url, const char *dest_path)
{
    if (!url || !dest_path) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Downloading image from %s to %s", url, dest_path);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err;
    if ((err = esp_http_client_open(client, 0)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "Failed to fetch headers");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    FILE *f = fopen(dest_path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open destination file for writing");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int total_read_len = 0, read_len;
    char *buffer = malloc(4096);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        fclose(f);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    while (1) {
        read_len = esp_http_client_read(client, buffer, 4096);
        if (read_len <= 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            ESP_LOGE(TAG, "Error reading data");
            free(buffer);
            fclose(f);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        fwrite(buffer, 1, read_len, f);
        total_read_len += read_len;
    }

    ESP_LOGI(TAG, "Download complete. Total size: %d bytes", total_read_len);

    free(buffer);
    fclose(f);
    esp_http_client_cleanup(client);
    return ESP_OK;
}
