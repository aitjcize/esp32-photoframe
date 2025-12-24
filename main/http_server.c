#include "http_server.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axp_prot.h"
#include "cJSON.h"
#include "config.h"
#include "display_manager.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "image_processor.h"
#include "power_manager.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char *TAG = "http_server";
static httpd_handle_t server = NULL;
static bool system_ready = false;

#define HTTPD_503 "503 Service Unavailable"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[] asm("_binary_style_css_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");
extern const uint8_t image_processor_js_start[] asm("_binary_image_processor_js_start");
extern const uint8_t image_processor_js_end[] asm("_binary_image_processor_js_end");
extern const uint8_t favicon_svg_start[] asm("_binary_favicon_svg_start");
extern const uint8_t favicon_svg_end[] asm("_binary_favicon_svg_end");

static esp_err_t index_handler(httpd_req_t *req)
{
    const size_t index_html_size = (index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *) index_html_start, index_html_size);
    return ESP_OK;
}

static esp_err_t style_handler(httpd_req_t *req)
{
    const size_t style_css_size = (style_css_end - style_css_start);
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *) style_css_start, style_css_size);
    return ESP_OK;
}

static esp_err_t app_js_handler(httpd_req_t *req)
{
    const size_t app_js_size = (app_js_end - app_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *) app_js_start, app_js_size);
    return ESP_OK;
}

static esp_err_t image_processor_js_handler(httpd_req_t *req)
{
    const size_t image_processor_js_size = (image_processor_js_end - image_processor_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *) image_processor_js_start, image_processor_js_size);
    return ESP_OK;
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    const size_t favicon_svg_size = (favicon_svg_end - favicon_svg_start);
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_send(req, (const char *) favicon_svg_start, favicon_svg_size);
    return ESP_OK;
}

static esp_err_t list_images_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    power_manager_reset_sleep_timer();

    DIR *dir = opendir(IMAGE_DIRECTORY);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open image directory");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *images = cJSON_CreateArray();

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            // Skip macOS resource fork files (._*)
            if (entry->d_name[0] == '.' && entry->d_name[1] == '_') {
                continue;
            }

            const char *ext = strrchr(entry->d_name, '.');
            if (ext && (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0)) {
                cJSON *image = cJSON_CreateObject();
                cJSON_AddStringToObject(image, "name", entry->d_name);

                char filepath[512];
                snprintf(filepath, sizeof(filepath), "%s/%s", IMAGE_DIRECTORY, entry->d_name);
                struct stat st;
                if (stat(filepath, &st) == 0) {
                    cJSON_AddNumberToObject(image, "size", st.st_size);
                }

                cJSON_AddItemToArray(images, image);
            }
        }
    }
    closedir(dir);

    cJSON_AddItemToObject(root, "images", images);

    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

static esp_err_t upload_image_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    power_manager_reset_sleep_timer();

    char *buf = malloc(8192);  // Larger buffer for better boundary detection
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    int buf_len = 0;  // Current data in buffer
    int remaining = req->content_len;

    ESP_LOGI(TAG, "Upload started, content length: %d", remaining);

    char boundary[128] = {0};
    size_t hdr_buf_len = sizeof(boundary);

    if (httpd_req_get_hdr_value_str(req, "Content-Type", boundary, hdr_buf_len) != ESP_OK) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No Content-Type header");
        return ESP_FAIL;
    }

    char *boundary_start = strstr(boundary, "boundary=");
    if (!boundary_start) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No boundary found");
        return ESP_FAIL;
    }

    boundary_start += 9;
    char *boundary_end = strchr(boundary_start, '\r');
    if (!boundary_end)
        boundary_end = strchr(boundary_start, '\n');
    if (!boundary_end)
        boundary_end = strchr(boundary_start, ';');
    if (!boundary_end)
        boundary_end = boundary_start + strlen(boundary_start);

    int boundary_value_len = boundary_end - boundary_start;

    // HTTP spec: actual boundary in data = "--" + boundary from header
    snprintf(boundary, sizeof(boundary), "--%.*s", boundary_value_len, boundary_start);
    int full_boundary_len = strlen(boundary);

    char filename[64] = {0};
    char current_field[64] = {0};
    char processing_mode[16] = "enhanced";  // Default to enhanced mode
    bool header_parsed = false;
    int file_count = 0;

    FILE *fp = NULL;
    char temp_fullsize_path[256];
    char temp_thumb_path[256];
    char final_bmp_path[256];
    char final_thumb_path[256];

    while (remaining > 0 || buf_len > 0) {
        // Read more data if buffer has space and there's data remaining
        if (remaining > 0 && buf_len < 4096) {
            int to_read = MIN(remaining, 8192 - buf_len);
            int received = httpd_req_recv(req, buf + buf_len, to_read);

            if (received <= 0) {
                if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                    continue;
                }
                if (fp)
                    fclose(fp);
                free(buf);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
                return ESP_FAIL;
            }

            buf_len += received;
            remaining -= received;
        }

        if (!header_parsed) {
            // Parse headers
            char *name_start = strstr(buf, "name=\"");
            if (name_start && name_start < buf + buf_len) {
                name_start += 6;
                char *name_end = strchr(name_start, '"');
                if (name_end && name_end < buf + buf_len) {
                    int name_len = name_end - name_start;
                    strncpy(current_field, name_start, MIN(name_len, sizeof(current_field) - 1));
                    current_field[MIN(name_len, sizeof(current_field) - 1)] = '\0';
                }
            }

            char *filename_start = strstr(buf, "filename=\"");
            if (filename_start && filename_start < buf + buf_len) {
                filename_start += 10;
                char *filename_end = strchr(filename_start, '"');
                if (filename_end && filename_end < buf + buf_len) {
                    int name_len = filename_end - filename_start;

                    if (file_count == 0) {
                        strncpy(filename, filename_start, MIN(name_len, sizeof(filename) - 1));
                        filename[MIN(name_len, sizeof(filename) - 1)] = '\0';

                        char *ext = strrchr(filename, '.');
                        if (!ext ||
                            (strcasecmp(ext, ".jpg") != 0 && strcasecmp(ext, ".jpeg") != 0)) {
                            if (fp)
                                fclose(fp);
                            free(buf);
                            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                                "Only JPG files are allowed");
                            return ESP_FAIL;
                        }
                    }

                    if (strcmp(current_field, "image") == 0) {
                        snprintf(temp_fullsize_path, sizeof(temp_fullsize_path), "%s/temp_full.jpg",
                                 IMAGE_DIRECTORY);
                        fp = fopen(temp_fullsize_path, "wb");
                    } else if (strcmp(current_field, "thumbnail") == 0) {
                        snprintf(temp_thumb_path, sizeof(temp_thumb_path), "%s/temp_thumb.jpg",
                                 IMAGE_DIRECTORY);
                        fp = fopen(temp_thumb_path, "wb");
                    }

                    if (!fp) {
                        free(buf);
                        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                            "Failed to create file");
                        return ESP_FAIL;
                    }
                    file_count++;
                }
            }

            // Find end of headers
            char *data_start = strstr(buf, "\r\n\r\n");
            if (data_start && data_start < buf + buf_len) {
                data_start += 4;
                int header_len = data_start - buf;
                header_parsed = true;

                // If this is the processingMode field, capture its value
                if (strcmp(current_field, "processingMode") == 0) {
                    // Find the end of the value (next boundary or CRLF)
                    char *value_end = strstr(data_start, "\r\n");
                    if (value_end && value_end < buf + buf_len) {
                        int value_len = value_end - data_start;
                        if (value_len > 0 && value_len < sizeof(processing_mode)) {
                            strncpy(processing_mode, data_start, value_len);
                            processing_mode[value_len] = '\0';
                            ESP_LOGI(TAG, "Processing mode: %s", processing_mode);
                        }
                    }
                }

                // Move remaining data to start of buffer
                buf_len -= header_len;
                memmove(buf, data_start, buf_len);
            } else if (remaining == 0) {
                // No more data coming and headers not complete
                break;
            }
        } else {
            // Look for boundary in current buffer
            char *boundary_pos = NULL;
            for (int i = 0; i <= buf_len - full_boundary_len; i++) {
                if (memcmp(buf + i, boundary, full_boundary_len) == 0) {
                    boundary_pos = buf + i;
                    break;
                }
            }

            if (boundary_pos) {
                // Found boundary
                int data_len = boundary_pos - buf;
                if (fp && data_len > 0) {
                    fwrite(buf, 1, data_len, fp);
                }

                if (fp) {
                    fclose(fp);
                    fp = NULL;
                }

                // Move past boundary
                int consumed = (boundary_pos - buf) + full_boundary_len;
                buf_len -= consumed;
                memmove(buf, buf + consumed, buf_len);

                // Reset for next part
                header_parsed = false;
                current_field[0] = '\0';
            } else {
                // No boundary found yet
                // Write data but keep last (boundary_len - 1) bytes in buffer
                int safe_write = buf_len - (full_boundary_len - 1);
                if (safe_write > 0 && remaining > 0) {
                    if (fp) {
                        fwrite(buf, 1, safe_write, fp);
                    }
                    buf_len -= safe_write;
                    memmove(buf, buf + safe_write, buf_len);
                } else if (remaining == 0 && buf_len > 0) {
                    // No more data coming, write everything
                    if (fp) {
                        fwrite(buf, 1, buf_len, fp);
                    }
                    buf_len = 0;
                }
            }
        }
    }

    if (fp) {
        fclose(fp);
        fp = NULL;
    }

    // Process uploaded files
    if (file_count >= 2) {
        ESP_LOGI(TAG, "Upload complete, converting full-size JPG to BMP");

        char bmp_filename[128];
        char jpg_filename[128];
        char *ext = strrchr(filename, '.');
        if (ext)
            *ext = '\0';

        snprintf(bmp_filename, sizeof(bmp_filename), "%s.bmp", filename);
        snprintf(jpg_filename, sizeof(jpg_filename), "%s.jpg", filename);
        snprintf(final_bmp_path, sizeof(final_bmp_path), "%s/%s", IMAGE_DIRECTORY, bmp_filename);
        snprintf(final_thumb_path, sizeof(final_thumb_path), "%s/%s", IMAGE_DIRECTORY,
                 jpg_filename);

        // Log memory before conversion
        ESP_LOGI(TAG, "Before conversion - Free heap: %lu bytes, Largest block: %lu bytes",
                 esp_get_free_heap_size(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

        // Convert full-size to BMP for display
        // Use stock mode if processing_mode is "stock", otherwise use enhanced mode
        bool use_stock_mode = (strcmp(processing_mode, "stock") == 0);
        ESP_LOGI(TAG, "Using %s mode for image processing", use_stock_mode ? "stock" : "enhanced");
        esp_err_t ret =
            image_processor_convert_jpg_to_bmp(temp_fullsize_path, final_bmp_path, use_stock_mode);

        // Log memory after conversion
        ESP_LOGI(TAG, "After conversion - Free heap: %lu bytes, Largest block: %lu bytes",
                 esp_get_free_heap_size(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

        if (ret != ESP_OK) {
            remove(temp_fullsize_path);
            remove(temp_thumb_path);
            free(buf);
            ESP_LOGE(TAG, "Failed to convert image");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to convert image");
            return ESP_FAIL;
        }

        // Delete full-size JPEG (no longer needed)
        remove(temp_fullsize_path);

        // Keep thumbnail JPEG
        rename(temp_thumb_path, final_thumb_path);

        ESP_LOGI(TAG, "Image converted successfully: %s (thumbnail: %s)", bmp_filename,
                 jpg_filename);

        cJSON *response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "filename", bmp_filename);

        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);

        free(json_str);
        cJSON_Delete(response);
        free(buf);

        return ESP_OK;
    }

    free(buf);
    ESP_LOGE(TAG, "Upload incomplete - expected 2 files, got %d", file_count);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Incomplete upload");
    return ESP_FAIL;
}

// URL decode helper function to handle encoded characters like %20 for space
static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t i = 0, j = 0;
    while (src[i] && j < dst_size - 1) {
        if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            // Convert hex to char
            char hex[3] = {src[i + 1], src[i + 2], '\0'};
            dst[j++] = (char) strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            // '+' is also used for space in query strings
            dst[j++] = ' ';
            i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

static esp_err_t serve_image_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    power_manager_reset_sleep_timer();

    // Get filename from query parameter
    char filename[128];
    size_t buf_len = sizeof(filename);

    if (httpd_req_get_url_query_str(req, filename, buf_len) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No filename specified");
        return ESP_FAIL;
    }

    // Extract 'name' parameter value
    char param_value[128];
    if (httpd_query_key_value(filename, "name", param_value, sizeof(param_value)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name parameter");
        return ESP_FAIL;
    }

    // URL decode the filename to handle spaces and special characters
    char decoded_filename[128];
    url_decode(decoded_filename, param_value, sizeof(decoded_filename));

    char filepath[256];
    const char *content_type = "image/jpeg";

    // Try to serve JPG thumbnail first
    snprintf(filepath, sizeof(filepath), "%s/%s", IMAGE_DIRECTORY, decoded_filename);

    FILE *fp = fopen(filepath, "rb");

    // If JPG doesn't exist and request was for .jpg, try .bmp fallback
    if (!fp) {
        char *ext = strrchr(decoded_filename, '.');
        if (ext && strcasecmp(ext, ".jpg") == 0) {
            // Convert .jpg to .bmp for fallback
            char bmp_filename[128];
            strncpy(bmp_filename, decoded_filename, sizeof(bmp_filename) - 1);
            char *bmp_ext = strrchr(bmp_filename, '.');
            if (bmp_ext) {
                strcpy(bmp_ext, ".bmp");
            }

            snprintf(filepath, sizeof(filepath), "%s/%s", IMAGE_DIRECTORY, bmp_filename);
            fp = fopen(filepath, "rb");
            if (fp) {
                content_type = "image/bmp";
                ESP_LOGW(TAG, "JPG thumbnail not found, serving BMP: %s", bmp_filename);
            }
        }
    }

    if (!fp) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Image not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type);
    // Cache images for 1 hour to reduce server load
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");

    char buffer[1024];
    size_t read_bytes;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK) {
            fclose(fp);
            return ESP_FAIL;
        }
    }

    fclose(fp);
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t delete_image_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    power_manager_reset_sleep_timer();

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *filename_obj = cJSON_GetObjectItem(root, "filename");
    if (!filename_obj || !cJSON_IsString(filename_obj)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
        return ESP_FAIL;
    }

    const char *filename = filename_obj->valuestring;

    // Copy filename to local buffer before deleting JSON
    char filename_copy[128];
    strncpy(filename_copy, filename, sizeof(filename_copy) - 1);
    filename_copy[sizeof(filename_copy) - 1] = '\0';

    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", IMAGE_DIRECTORY, filename_copy);

    // Also delete the corresponding JPEG thumbnail
    char jpg_filename[128];
    strncpy(jpg_filename, filename_copy, sizeof(jpg_filename) - 1);
    jpg_filename[sizeof(jpg_filename) - 1] = '\0';
    char *ext = strrchr(jpg_filename, '.');
    if (ext && strcasecmp(ext, ".bmp") == 0) {
        strcpy(ext, ".jpg");
    }

    char jpg_path[256];
    snprintf(jpg_path, sizeof(jpg_path), "%s/%s", IMAGE_DIRECTORY, jpg_filename);

    // Delete JSON before file operations
    cJSON_Delete(root);

    if (unlink(filepath) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete file");
        return ESP_FAIL;
    }

    // Delete thumbnail (ignore errors if it doesn't exist)
    unlink(jpg_path);

    ESP_LOGI(TAG, "Image deleted successfully: %s", filename_copy);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    return ESP_OK;
}

static esp_err_t display_image_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    power_manager_reset_sleep_timer();

    // Check if display is already busy
    if (display_manager_is_busy()) {
        ESP_LOGW(TAG, "Display is busy, rejecting request");
        cJSON *response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "status", "busy");
        cJSON_AddStringToObject(response, "message", "Display is currently updating, please wait");

        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, json_str);

        free(json_str);
        cJSON_Delete(response);
        return ESP_OK;
    }

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *filename_obj = cJSON_GetObjectItem(root, "filename");
    if (!filename_obj || !cJSON_IsString(filename_obj)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
        return ESP_FAIL;
    }

    const char *filename = filename_obj->valuestring;

    // Build absolute path
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "/sdcard/images/%s", filename);

    esp_err_t err = display_manager_show_image(filepath);

    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to display image");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");

    esp_err_t send_err = httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    if (send_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send response (connection likely closed): %d", send_err);
    }

    return ESP_OK;
}

static esp_err_t display_image_direct_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    power_manager_reset_sleep_timer();

    // Check if display is already busy
    if (display_manager_is_busy()) {
        ESP_LOGW(TAG, "Display is busy, rejecting request");
        cJSON *response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "status", "busy");
        cJSON_AddStringToObject(response, "message", "Display is currently updating, please wait");

        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, json_str);

        free(json_str);
        cJSON_Delete(response);
        return ESP_OK;
    }

    // Get content length
    size_t content_len = req->content_len;
    const size_t MAX_UPLOAD_SIZE = 5 * 1024 * 1024;  // 5MB max for JPEG file

    if (content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request body");
        return ESP_FAIL;
    }

    if (content_len > MAX_UPLOAD_SIZE) {
        ESP_LOGW(TAG, "Upload rejected: %zu bytes exceeds limit of %zu bytes", content_len,
                 MAX_UPLOAD_SIZE);
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "File too large: %zu KB (max: %zu KB). Please compress or resize your image.",
                 content_len / 1024, MAX_UPLOAD_SIZE / 1024);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error_msg);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Receiving JPG image for direct display, size: %zu bytes (%.1f KB)", content_len,
             content_len / 1024.0);

    // Create temporary file path in /sdcard root
    const char *temp_jpg_path = "/sdcard/.temp_upload.jpg";
    const char *temp_bmp_path = "/sdcard/.temp_upload.bmp";

    // Delete old temp files to prevent caching issues
    unlink(temp_jpg_path);
    unlink(temp_bmp_path);

    // Open file for writing
    FILE *fp = fopen(temp_jpg_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to create temporary file");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Failed to create temporary file");
        return ESP_FAIL;
    }

    // Receive and write data in chunks
    char *buf = malloc(4096);
    if (!buf) {
        fclose(fp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < content_len) {
        size_t to_read = MIN(4096, content_len - received);
        int ret = httpd_req_recv(req, buf, to_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Failed to receive data");
            free(buf);
            fclose(fp);
            unlink(temp_jpg_path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
            return ESP_FAIL;
        }
        fwrite(buf, 1, ret, fp);
        received += ret;
    }

    free(buf);
    fclose(fp);

    ESP_LOGI(TAG, "JPG image received successfully, processing...");

    // Convert JPG to BMP using image processor
    esp_err_t err = image_processor_convert_jpg_to_bmp(temp_jpg_path, temp_bmp_path, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to convert JPG to BMP: %s", esp_err_to_name(err));
        unlink(temp_jpg_path);

        // Provide specific error messages based on error type
        if (err == ESP_ERR_INVALID_SIZE) {
            httpd_resp_send_err(
                req, HTTPD_400_BAD_REQUEST,
                "Image is too large (max: 6400x3840). Please resize your image and try again.");
        } else if (err == ESP_ERR_NO_MEM) {
            httpd_resp_send_err(
                req, HTTPD_400_BAD_REQUEST,
                "Image requires too much memory to process. Please use a smaller image.");
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to process image");
        }
        return ESP_FAIL;
    }

    // Display the converted BMP (use full path to bypass IMAGE_DIRECTORY prefix)
    err = display_manager_show_image("/sdcard/.temp_upload.bmp");

    // Clean up temporary files
    unlink(temp_jpg_path);
    // Keep the BMP file for now in case we want to redisplay it

    if (err != ESP_OK) {
        unlink(temp_bmp_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to display image");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddStringToObject(response, "message", "Image displayed successfully");

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");

    esp_err_t send_err = httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    if (send_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send response (connection likely closed): %d", send_err);
    }

    return ESP_OK;
}

static esp_err_t battery_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    power_manager_reset_sleep_timer();

    cJSON *response = cJSON_CreateObject();

    int battery_voltage = axp_get_battery_voltage();
    int battery_percent = axp_get_battery_percent();
    bool is_charging = axp_is_charging();
    bool usb_connected = axp_is_usb_connected();
    bool battery_connected = axp_is_battery_connected();

    cJSON_AddNumberToObject(response, "battery_voltage_mv", battery_voltage);
    cJSON_AddNumberToObject(response, "battery_percent", battery_percent);
    cJSON_AddBoolToObject(response, "charging", is_charging);
    cJSON_AddBoolToObject(response, "usb_connected", usb_connected);
    cJSON_AddBoolToObject(response, "battery_connected", battery_connected);

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    return ESP_OK;
}

static void delayed_sleep_task(void *arg)
{
    // Wait for HTTP response to be sent
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Delayed sleep task: entering sleep now");
    power_manager_enter_sleep();

    // Task will be deleted when device enters deep sleep
    vTaskDelete(NULL);
}

static esp_err_t sleep_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddStringToObject(response, "message", "Entering sleep mode");

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    // Create a task to enter sleep after HTTP response completes
    xTaskCreate(delayed_sleep_task, "delayed_sleep", 2048, NULL, 5, NULL);

    return ESP_OK;
}

static esp_err_t config_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    power_manager_reset_sleep_timer();

    if (req->method == HTTP_GET) {
        int rotate_interval = display_manager_get_rotate_interval();
        bool auto_rotate = display_manager_get_auto_rotate();

        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "rotate_interval", rotate_interval);
        cJSON_AddBoolToObject(root, "auto_rotate", auto_rotate);

        char *json_str = cJSON_Print(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);

        free(json_str);
        cJSON_Delete(root);

        return ESP_OK;
    } else if (req->method == HTTP_POST) {
        char buf[256];
        int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
            return ESP_FAIL;
        }
        buf[ret] = '\0';

        cJSON *root = cJSON_Parse(buf);
        if (!root) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            return ESP_FAIL;
        }

        cJSON *interval_obj = cJSON_GetObjectItem(root, "rotate_interval");
        if (interval_obj && cJSON_IsNumber(interval_obj)) {
            display_manager_set_rotate_interval(interval_obj->valueint);
            power_manager_reset_rotate_timer();
        }

        cJSON *auto_rotate_obj = cJSON_GetObjectItem(root, "auto_rotate");
        if (auto_rotate_obj && cJSON_IsBool(auto_rotate_obj)) {
            display_manager_set_auto_rotate(cJSON_IsTrue(auto_rotate_obj));
        }

        cJSON_Delete(root);

        cJSON *response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "status", "success");

        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);

        free(json_str);
        cJSON_Delete(response);

        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

esp_err_t http_server_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.stack_size = 12288;  // Increased from 8192 to 12KB
    // config.max_open_sockets = 4;  // Limit concurrent connections to prevent memory exhaustion

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = {
            .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &index_uri);

        httpd_uri_t style_uri = {
            .uri = "/style.css", .method = HTTP_GET, .handler = style_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &style_uri);

        httpd_uri_t app_js_uri = {
            .uri = "/app.js", .method = HTTP_GET, .handler = app_js_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &app_js_uri);

        httpd_uri_t image_processor_js_uri = {.uri = "/image-processor.js",
                                              .method = HTTP_GET,
                                              .handler = image_processor_js_handler,
                                              .user_ctx = NULL};
        httpd_register_uri_handler(server, &image_processor_js_uri);

        httpd_uri_t favicon_uri = {.uri = "/favicon.svg",
                                   .method = HTTP_GET,
                                   .handler = favicon_handler,
                                   .user_ctx = NULL};
        httpd_register_uri_handler(server, &favicon_uri);

        httpd_uri_t list_uri = {.uri = "/api/images",
                                .method = HTTP_GET,
                                .handler = list_images_handler,
                                .user_ctx = NULL};
        httpd_register_uri_handler(server, &list_uri);

        httpd_uri_t upload_uri = {.uri = "/api/upload",
                                  .method = HTTP_POST,
                                  .handler = upload_image_handler,
                                  .user_ctx = NULL};
        httpd_register_uri_handler(server, &upload_uri);

        httpd_uri_t display_uri = {.uri = "/api/display",
                                   .method = HTTP_POST,
                                   .handler = display_image_handler,
                                   .user_ctx = NULL};
        httpd_register_uri_handler(server, &display_uri);

        httpd_uri_t display_image_direct_uri = {.uri = "/api/display-image",
                                                .method = HTTP_POST,
                                                .handler = display_image_direct_handler,
                                                .user_ctx = NULL};
        httpd_register_uri_handler(server, &display_image_direct_uri);

        httpd_uri_t delete_uri = {.uri = "/api/delete",
                                  .method = HTTP_POST,
                                  .handler = delete_image_handler,
                                  .user_ctx = NULL};
        httpd_register_uri_handler(server, &delete_uri);

        httpd_uri_t serve_image_uri = {.uri = "/api/image",
                                       .method = HTTP_GET,
                                       .handler = serve_image_handler,
                                       .user_ctx = NULL};
        httpd_register_uri_handler(server, &serve_image_uri);

        httpd_uri_t config_get_uri = {
            .uri = "/api/config", .method = HTTP_GET, .handler = config_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &config_get_uri);

        httpd_uri_t config_post_uri = {
            .uri = "/api/config", .method = HTTP_POST, .handler = config_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &config_post_uri);

        httpd_uri_t battery_uri = {.uri = "/api/battery",
                                   .method = HTTP_GET,
                                   .handler = battery_handler,
                                   .user_ctx = NULL};
        httpd_register_uri_handler(server, &battery_uri);

        httpd_uri_t sleep_uri = {
            .uri = "/api/sleep", .method = HTTP_POST, .handler = sleep_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &sleep_uri);

        ESP_LOGI(TAG, "HTTP server started");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to start HTTP server");
    return ESP_FAIL;
}

esp_err_t http_server_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    return ESP_OK;
}

void http_server_set_ready(void)
{
    system_ready = true;
    ESP_LOGI(TAG, "System marked as ready for HTTP requests");
}
