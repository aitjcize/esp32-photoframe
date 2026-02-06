#include "ai_manager.h"

#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "config.h"
#include "config_manager.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "image_processor.h"
#include "mbedtls/base64.h"
#include "processing_settings.h"
#ifdef CONFIG_HAS_SDCARD
#include "sdcard.h"
#endif
#include "wifi_manager.h"

static const char *TAG = "ai_manager";

// Embedded OpenAI Root Certificate
extern const uint8_t openai_root_pem_start[] asm("_binary_openai_root_pem_start");
extern const uint8_t openai_root_pem_end[] asm("_binary_openai_root_pem_end");

static ai_generation_status_t current_status = AI_STATUS_IDLE;
static char last_image_path[128] = {0};
static char last_error[256] = {0};
static char current_prompt[AI_PROMPT_MAX_LEN] = {0};
static char current_model[64] = {0};

static TaskHandle_t ai_task_handle = NULL;
static SemaphoreHandle_t generation_trigger = NULL;

static void ai_task(void *pvParameters);
static esp_err_t generate_openai(const char *prompt, const char *model, const char *dest_path);
static esp_err_t download_image(const char *url, const char *dest_path);

esp_err_t ai_manager_init(void)
{
    generation_trigger = xSemaphoreCreateBinary();
    if (generation_trigger == NULL) {
        ESP_LOGE(TAG, "Failed to create generation semaphore");
        return ESP_FAIL;
    }

    BaseType_t ret = xTaskCreate(ai_task, "ai_task", 8192, NULL, 5, &ai_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create AI task");
        vSemaphoreDelete(generation_trigger);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AI Manager initialized");
    return ESP_OK;
}

esp_err_t ai_manager_generate(const char *prompt_override)
{
    if (current_status != AI_STATUS_IDLE && current_status != AI_STATUS_COMPLETE &&
        current_status != AI_STATUS_ERROR) {
        ESP_LOGW(TAG, "Generation already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    // Determine prompt
    if (prompt_override && strlen(prompt_override) > 0) {
        strncpy(current_prompt, prompt_override, sizeof(current_prompt) - 1);
    } else {
        const char *config_prompt = config_manager_get_ai_prompt();
        if (config_prompt && strlen(config_prompt) > 0) {
            strncpy(current_prompt, config_prompt, sizeof(current_prompt) - 1);
        } else {
            strncpy(current_prompt, "A random artistic image", sizeof(current_prompt) - 1);
        }
    }
    current_prompt[sizeof(current_prompt) - 1] = '\0';

    // Get model from config
    const char *config_model = config_manager_get_ai_model();
    strncpy(current_model, config_model, sizeof(current_model) - 1);
    current_model[sizeof(current_model) - 1] = '\0';

    current_status = AI_STATUS_IDLE;  // Reset status
    xSemaphoreGive(generation_trigger);
    return ESP_OK;
}

static void ai_task(void *pvParameters)
{
    while (1) {
        if (xSemaphoreTake(generation_trigger, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Starting AI generation with prompt: %s", current_prompt);
            current_status = AI_STATUS_GENERATING;
            last_error[0] = '\0';

            // Construct destination path
            snprintf(last_image_path, sizeof(last_image_path), "%s/ai_generated.jpg",
                     TEMP_MOUNT_POINT);

            // Always use OpenAI for now
            esp_err_t err = generate_openai(current_prompt, current_model, last_image_path);

            if (err == ESP_OK) {
                // Process image (Dither + Resize -> CURRENT_PNG_PATH)
                ESP_LOGI(TAG, "Processing generated image...");

                processing_settings_t settings;
                if (processing_settings_load(&settings) != ESP_OK) {
                    processing_settings_get_defaults(&settings);
                }
                bool use_stock_mode = (strcmp(settings.processing_mode, "stock") == 0);
                dither_algorithm_t algo = processing_settings_get_dithering_algorithm();

                err = image_processor_process(last_image_path, CURRENT_PNG_PATH, use_stock_mode,
                                              algo);

                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Image processed successfully");

#ifdef CONFIG_HAS_SDCARD
                    // Save the processed image to downloads if enabled
                    if (config_manager_get_save_downloaded_images() && sdcard_is_mounted()) {
                        char dest_path[256];
                        time_t now;
                        time(&now);
                        struct tm timeinfo;
                        localtime_r(&now, &timeinfo);

                        // Create directory if needed
                        struct stat st;
                        if (stat(DOWNLOAD_DIRECTORY, &st) != 0)
                            mkdir(DOWNLOAD_DIRECTORY, 0755);

                        snprintf(dest_path, sizeof(dest_path),
                                 DOWNLOAD_DIRECTORY "/ai_%04d%02d%02d_%02d%02d%02d.png",
                                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

                        // Move the processed image to the permanent location
                        if (rename(CURRENT_PNG_PATH, dest_path) == 0) {
                            ESP_LOGI(TAG, "Saved processed AI image to: %s", dest_path);
                            strncpy(last_image_path, dest_path, sizeof(last_image_path) - 1);
                        } else {
                            ESP_LOGE(TAG, "Failed to save processed AI image to Downloads");
                            strncpy(last_image_path, CURRENT_PNG_PATH, sizeof(last_image_path) - 1);
                        }
                    } else {
                        strncpy(last_image_path, CURRENT_PNG_PATH, sizeof(last_image_path) - 1);
                    }
#else
                    strncpy(last_image_path, CURRENT_PNG_PATH, sizeof(last_image_path) - 1);
#endif
                    current_status = AI_STATUS_COMPLETE;
                } else {
                    ESP_LOGE(TAG, "Image processing failed");
                    snprintf(last_error, sizeof(last_error), "Processing failed");
                    current_status = AI_STATUS_ERROR;
                }
            } else {
                ESP_LOGE(TAG, "Generation failed");
                if (last_error[0] == '\0') {
                    snprintf(last_error, sizeof(last_error), "Process failed");
                }
                current_status = AI_STATUS_ERROR;
            }
        }
    }
}

static esp_err_t decode_and_save_b64(const char *b64_str, const char *dest_path)
{
    size_t b64_len = strlen(b64_str);
    size_t decoded_len = 0;
    unsigned char *decoded_data = malloc(b64_len);
    if (!decoded_data)
        return ESP_ERR_NO_MEM;

    esp_err_t err = ESP_FAIL;
    if (mbedtls_base64_decode(decoded_data, b64_len, &decoded_len, (const unsigned char *) b64_str,
                              b64_len) == 0) {
        FILE *fp = fopen(dest_path, "wb");
        if (fp) {
            if (fwrite(decoded_data, 1, decoded_len, fp) == decoded_len) {
                err = ESP_OK;
                ESP_LOGI(TAG, "Saved %zu bytes to %s", decoded_len, dest_path);
            }
            fclose(fp);
        }
    }
    free(decoded_data);
    return err;
}

static esp_err_t parse_openai_response(const char *json_buf, const char *dest_path)
{
    cJSON *resp_json = cJSON_Parse(json_buf);
    if (!resp_json)
        return ESP_FAIL;

    esp_err_t err = ESP_FAIL;
    cJSON *data = cJSON_GetObjectItem(resp_json, "data");
    cJSON *first = cJSON_IsArray(data) ? cJSON_GetArrayItem(data, 0) : NULL;

    if (first) {
        cJSON *url = cJSON_GetObjectItem(first, "url");
        cJSON *b64 = cJSON_GetObjectItem(first, "b64_json");

        if (cJSON_IsString(url) && url->valuestring) {
            current_status = AI_STATUS_DOWNLOADING;
            err = download_image(url->valuestring, dest_path);
        } else if (cJSON_IsString(b64) && b64->valuestring) {
            current_status = AI_STATUS_DOWNLOADING;
            err = decode_and_save_b64(b64->valuestring, dest_path);
        }
    }

    cJSON_Delete(resp_json);
    return err;
}

static esp_err_t generate_openai(const char *prompt, const char *model, const char *dest_path)
{
    const char *api_key = config_manager_get_openai_api_key();
    if (!api_key || strlen(api_key) == 0) {
        snprintf(last_error, sizeof(last_error), "API Key missing");
        return ESP_FAIL;
    }

    esp_http_client_config_t config = {
        .url = "https://api.openai.com/v1/images/generations",
        .method = HTTP_METHOD_POST,
        .cert_pem = (const char *) openai_root_pem_start,
        .timeout_ms = 90000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
        return ESP_FAIL;

    esp_err_t ret = ESP_FAIL;
    char *json_str = NULL;
    cJSON *root = NULL;

    char auth_header[AI_API_KEY_MAX_LEN + 16];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddStringToObject(root, "prompt", prompt);
    cJSON_AddNumberToObject(root, "n", 1);

    display_orientation_t orient = config_manager_get_display_orientation();
    cJSON_AddStringToObject(root, "size",
                            orient == DISPLAY_ORIENTATION_PORTRAIT ? "1024x1536" : "1536x1024");
    cJSON_AddStringToObject(root, "quality", "high");
    cJSON_AddStringToObject(root, "output_format", "jpeg");
    cJSON_AddNumberToObject(root, "output_compression", 90);

    json_str = cJSON_PrintUnformatted(root);
    if (!json_str)
        goto cleanup;

    if (esp_http_client_open(client, strlen(json_str)) != ESP_OK) {
        goto cleanup;
    }

    if (esp_http_client_write(client, json_str, strlen(json_str)) < 0) {
        goto cleanup;
    }

    esp_http_client_fetch_headers(client);

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        snprintf(last_error, sizeof(last_error), "API Error: %d", status_code);
        goto cleanup;
    }

    const size_t resp_size = 2 * 1024 * 1024;
    char *response_buf = heap_caps_malloc(resp_size, MALLOC_CAP_SPIRAM);
    if (response_buf) {
        int total_read = 0;
        while (total_read < resp_size - 1) {
            int read_len =
                esp_http_client_read(client, response_buf + total_read, resp_size - total_read - 1);
            if (read_len <= 0)
                break;
            total_read += read_len;
        }
        response_buf[total_read] = '\0';
        if (total_read > 0) {
            ret = parse_openai_response(response_buf, dest_path);
        }
        heap_caps_free(response_buf);
    }

cleanup:
    if (root)
        cJSON_Delete(root);
    if (json_str)
        free(json_str);
    esp_http_client_cleanup(client);
    return ret;
}

static esp_err_t download_image(const char *url, const char *dest_path)
{
    esp_http_client_config_t config = {
        .url = url,
        .skip_cert_common_name_check = true,
        .timeout_ms = 60000,
        .buffer_size = 16384,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
        return ESP_FAIL;

    FILE *fp = fopen(dest_path, "wb");
    if (!fp) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK || esp_http_client_fetch_headers(client) < 0) {
        err = ESP_FAIL;
        goto cleanup;
    }

    int content_length = esp_http_client_get_content_length(client);
    char *buffer = malloc(content_length);
    if (!buffer) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    int total_read = 0;
    while (1) {
        int read_len = esp_http_client_read(client, buffer, 4096);
        if (read_len <= 0)
            break;
        fwrite(buffer, 1, read_len, fp);
        total_read += read_len;
    }
    free(buffer);
    ESP_LOGI(TAG, "Downloaded %d bytes", total_read);

cleanup:
    fclose(fp);
    esp_http_client_cleanup(client);
    return err;
}

ai_generation_status_t ai_manager_get_status(void)
{
    return current_status;
}

const char *ai_manager_get_last_image_path(void)
{
    return last_image_path;
}

const char *ai_manager_get_last_error(void)
{
    return last_error;
}