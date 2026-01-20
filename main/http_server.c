#include "http_server.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "album_manager.h"
#include "axp_prot.h"
#include "cJSON.h"
#include "color_palette.h"
#include "config.h"
#include "config_manager.h"
#include "display_manager.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "ha_integration.h"
#include "image_processor.h"
#include "ota_manager.h"
#include "power_manager.h"
#include "processing_settings.h"
#include "utils.h"

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
extern const uint8_t calibration_bmp_start[] asm("_binary_calibration_bmp_start");
extern const uint8_t calibration_bmp_end[] asm("_binary_calibration_bmp_end");
extern const uint8_t measurement_sample_jpg_start[] asm("_binary_measurement_sample_jpg_start");
extern const uint8_t measurement_sample_jpg_end[] asm("_binary_measurement_sample_jpg_end");

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

static esp_err_t measurement_sample_handler(httpd_req_t *req)
{
    const size_t measurement_sample_jpg_size =
        (measurement_sample_jpg_end - measurement_sample_jpg_start);
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_send(req, (const char *) measurement_sample_jpg_start, measurement_sample_jpg_size);
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

    char *buf = malloc(4096);  // 4KB buffer - balance between performance and memory usage
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
    char album_name[128] = DEFAULT_ALBUM_NAME;
    bool header_parsed = false;
    int file_count = 0;

    FILE *fp = NULL;
    char temp_fullsize_path[512];
    char temp_thumb_path[512];
    char final_bmp_path[512];
    char final_thumb_path[512];
    char album_path[256];

    while (remaining > 0 || buf_len > 0) {
        // Read more data if buffer has space and there's data remaining
        if (remaining > 0 && buf_len < 2048) {
            int to_read = MIN(remaining, 4096 - buf_len);
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
                        album_manager_get_album_path(album_name, album_path, sizeof(album_path));

                        // Ensure album directory exists
                        struct stat st;
                        if (stat(album_path, &st) != 0) {
                            ESP_LOGI(TAG, "Creating album directory: %s", album_path);
                            if (mkdir(album_path, 0755) != 0) {
                                ESP_LOGE(TAG, "Failed to create directory: %s, errno: %d",
                                         album_path, errno);
                                free(buf);
                                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                                    "Failed to create album directory");
                                return ESP_FAIL;
                            }
                        }

                        snprintf(temp_fullsize_path, sizeof(temp_fullsize_path), "%s/temp_full.jpg",
                                 album_path);
                        fp = fopen(temp_fullsize_path, "wb");
                        if (!fp) {
                            free(buf);
                            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                                "Failed to create file");
                            return ESP_FAIL;
                        }
                        file_count++;
                    } else if (strcmp(current_field, "thumbnail") == 0) {
                        album_manager_get_album_path(album_name, album_path, sizeof(album_path));
                        snprintf(temp_thumb_path, sizeof(temp_thumb_path), "%s/temp_thumb.jpg",
                                 album_path);
                        fp = fopen(temp_thumb_path, "wb");
                        if (!fp) {
                            free(buf);
                            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                                "Failed to create file");
                            return ESP_FAIL;
                        }
                        file_count++;
                    }
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

                // If this is the album field, capture its value
                if (strcmp(current_field, "album") == 0) {
                    char *value_end = strstr(data_start, "\r\n");
                    if (value_end && value_end < buf + buf_len) {
                        int value_len = value_end - data_start;
                        if (value_len > 0 && value_len < sizeof(album_name)) {
                            strncpy(album_name, data_start, value_len);
                            album_name[value_len] = '\0';
                            ESP_LOGI(TAG, "Album: %s", album_name);
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
        album_manager_get_album_path(album_name, album_path, sizeof(album_path));
        snprintf(final_bmp_path, sizeof(final_bmp_path), "%s/%s", album_path, bmp_filename);
        snprintf(final_thumb_path, sizeof(final_thumb_path), "%s/%s", album_path, jpg_filename);

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
    char decoded_filename[256];
    url_decode(decoded_filename, param_value, sizeof(decoded_filename));

    char filepath[512];
    const char *content_type = "image/jpeg";

    // Filename can be "album/file.jpg" or just "file.jpg"
    // Build full path: /sdcard/images/ + decoded_filename
    snprintf(filepath, sizeof(filepath), "%s/%s", IMAGE_DIRECTORY, decoded_filename);

    FILE *fp = fopen(filepath, "rb");

    // If JPG doesn't exist and request was for .jpg, try .bmp fallback
    if (!fp) {
        char *ext = strrchr(decoded_filename, '.');
        if (ext && strcasecmp(ext, ".jpg") == 0) {
            // Convert .jpg to .bmp for fallback
            char bmp_filename[256];
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
    char filename_copy[256];
    strncpy(filename_copy, filename, sizeof(filename_copy) - 1);
    filename_copy[sizeof(filename_copy) - 1] = '\0';

    // Build path - filename can be "album/file.bmp" or just "file.bmp"
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", IMAGE_DIRECTORY, filename_copy);

    // Also delete the corresponding JPEG thumbnail
    char jpg_filename[256];
    strncpy(jpg_filename, filename_copy, sizeof(jpg_filename) - 1);
    jpg_filename[sizeof(jpg_filename) - 1] = '\0';
    char *ext = strrchr(jpg_filename, '.');
    if (ext && strcasecmp(ext, ".bmp") == 0) {
        strcpy(ext, ".jpg");
    }

    char jpg_path[512];
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

    // Build absolute path - filename can be "album/file.bmp" or just "file.bmp"
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", IMAGE_DIRECTORY, filename);

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

    // Get content type to determine if it's JPG or BMP
    char content_type[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) !=
        ESP_OK) {
        strcpy(content_type, "image/jpeg");  // Default to JPEG
    }
    bool upload_is_bmp = (strcmp(content_type, "image/bmp") == 0);

    // Get content length
    size_t content_len = req->content_len;
    const size_t MAX_UPLOAD_SIZE = 5 * 1024 * 1024;  // 5MB max

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

    ESP_LOGI(TAG, "Receiving %s image for direct display, size: %zu bytes (%.1f KB)",
             upload_is_bmp ? "BMP" : "JPG", content_len, content_len / 1024.0);

    // Use fixed paths for current image and thumbnail
    const char *temp_upload_path = CURRENT_UPLOAD_PATH;
    const char *temp_jpg_path = CURRENT_JPG_PATH;
    const char *temp_bmp_path = CURRENT_BMP_PATH;

    // Delete old files to prevent caching issues
    unlink(temp_upload_path);
    unlink(temp_jpg_path);
    unlink(temp_bmp_path);

    // Open file for writing
    FILE *fp = fopen(temp_upload_path, "wb");
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
            unlink(temp_upload_path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
            return ESP_FAIL;
        }
        fwrite(buf, 1, ret, fp);
        received += ret;
    }

    free(buf);
    fclose(fp);

    ESP_LOGI(TAG, "Image received successfully, processing...");

    esp_err_t err = ESP_OK;
    if (upload_is_bmp) {
        // Move uploaded BMP to temp location
        if (rename(temp_upload_path, temp_bmp_path) != 0) {
            ESP_LOGE(TAG, "Failed to move uploaded BMP to temp location");
            unlink(temp_upload_path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to process BMP");
            return ESP_FAIL;
        }
    } else {
        // Convert JPG to BMP using image processor
        err = image_processor_convert_jpg_to_bmp(temp_upload_path, temp_bmp_path, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to convert JPG to BMP: %s", esp_err_to_name(err));
            unlink(temp_upload_path);

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
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                    "Failed to process image");
            }
            return ESP_FAIL;
        }
    }

    // Display the BMP (use full path to bypass IMAGE_DIRECTORY prefix)
    err = display_manager_show_image(temp_bmp_path);

    if (err != ESP_OK) {
        unlink(temp_upload_path);
        unlink(temp_bmp_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to display image");
        return ESP_FAIL;
    }

    if (upload_is_bmp) {
        // BMP upload - no separate thumbnail
        ESP_LOGI(TAG, "BMP image saved: %s", temp_bmp_path);
    } else {
        // JPG upload - move to thumbnail location for HA integration
        unlink(temp_jpg_path);
        if (rename(temp_upload_path, temp_jpg_path) != 0) {
            ESP_LOGW(TAG, "Failed to save JPG thumbnail");
        } else {
            // Keep both .current.bmp and .current.jpg for HA integration
            // .current.bmp is referenced by current_image
            // .current.jpg is the thumbnail served by /api/current_image
            ESP_LOGI(TAG, "Image and thumbnail saved: %s, %s", temp_bmp_path, temp_jpg_path);
        }
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

    cJSON *response = create_battery_json();
    if (response == NULL) {
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_sendstr(req, "Failed to create battery JSON");
        return ESP_FAIL;
    }

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

static esp_err_t rotate_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Manual rotation triggered via API");

    power_manager_reset_sleep_timer();
    trigger_image_rotation();

    // Notify HA that image has been updated
    ha_notify_update();

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddStringToObject(response, "message", "Image rotation triggered");

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    return ESP_OK;
}

static esp_err_t current_image_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    char image_to_serve[512] = {0};
    const char *content_type = "image/jpeg";

    // Read image path from .current.lnk
    FILE *link_fp = fopen(CURRENT_IMAGE_LINK, "r");
    if (!link_fp) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No image currently displayed");
        return ESP_FAIL;
    }

    // Read the path from link file
    if (!fgets(image_to_serve, sizeof(image_to_serve), link_fp)) {
        fclose(link_fp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read link file");
        return ESP_FAIL;
    }
    fclose(link_fp);

    // Remove trailing newline if present
    size_t len = strlen(image_to_serve);
    if (len > 0 && image_to_serve[len - 1] == '\n') {
        image_to_serve[len - 1] = '\0';
    }

    ESP_LOGI(TAG, "Serving image from link: %s", image_to_serve);

    // Try to serve JPG version first by replacing .bmp extension with .jpg
    char thumbnail_path[512];
    strncpy(thumbnail_path, image_to_serve, sizeof(thumbnail_path) - 1);
    thumbnail_path[sizeof(thumbnail_path) - 1] = '\0';

    char *ext = strrchr(thumbnail_path, '.');
    if (ext && strcasecmp(ext, ".bmp") == 0) {
        strcpy(ext, ".jpg");
    }

    FILE *fp = fopen(thumbnail_path, "rb");

    if (!fp) {
        // Fall back to BMP if JPG doesn't exist
        fp = fopen(image_to_serve, "rb");
        content_type = "image/bmp";

        if (!fp) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Image not found");
            return ESP_FAIL;
        }
    }

    httpd_resp_set_type(req, content_type);
    // Cache for 30 seconds since current image changes infrequently
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=30");

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

static esp_err_t config_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    if (req->method == HTTP_GET) {
        int rotate_interval = config_manager_get_rotate_interval();
        int image_orientation = config_manager_get_image_orientation();
        bool auto_rotate = config_manager_get_auto_rotate();
        bool deep_sleep_enabled = power_manager_get_deep_sleep_enabled();
        const char *image_url = config_manager_get_image_url();
        const char *ha_url = config_manager_get_ha_url();
        rotation_mode_t rotation_mode = config_manager_get_rotation_mode();
        bool save_downloaded_images = config_manager_get_save_downloaded_images();
        display_orientation_t display_orientation = config_manager_get_display_orientation();

        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "rotate_interval", rotate_interval);
        cJSON_AddNumberToObject(root, "image_orientation", image_orientation);
        cJSON_AddBoolToObject(root, "auto_rotate", auto_rotate);
        cJSON_AddBoolToObject(root, "deep_sleep_enabled", deep_sleep_enabled);
        cJSON_AddStringToObject(root, "image_url", image_url ? image_url : "");
        cJSON_AddStringToObject(root, "ha_url", ha_url ? ha_url : "");
        cJSON_AddStringToObject(root, "rotation_mode",
                                rotation_mode == ROTATION_MODE_URL ? "url" : "sdcard");
        cJSON_AddBoolToObject(root, "save_downloaded_images", save_downloaded_images);
        cJSON_AddStringToObject(
            root, "display_orientation",
            display_orientation == DISPLAY_ORIENTATION_LANDSCAPE ? "landscape" : "portrait");

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
            config_manager_set_rotate_interval(interval_obj->valueint);
            power_manager_reset_rotate_timer();
        }

        cJSON *img_orientation_obj = cJSON_GetObjectItem(root, "image_orientation");
        if (img_orientation_obj && cJSON_IsNumber(img_orientation_obj)) {
            config_manager_set_image_orientation(img_orientation_obj->valueint);
            display_manager_initialize_paint();
        }

        cJSON *auto_rotate_obj = cJSON_GetObjectItem(root, "auto_rotate");
        if (auto_rotate_obj && cJSON_IsBool(auto_rotate_obj)) {
            config_manager_set_auto_rotate(cJSON_IsTrue(auto_rotate_obj));
        }

        cJSON *deep_sleep_obj = cJSON_GetObjectItem(root, "deep_sleep_enabled");
        if (deep_sleep_obj && cJSON_IsBool(deep_sleep_obj)) {
            power_manager_set_deep_sleep_enabled(cJSON_IsTrue(deep_sleep_obj));
        }

        cJSON *image_url_obj = cJSON_GetObjectItem(root, "image_url");
        if (image_url_obj && cJSON_IsString(image_url_obj)) {
            const char *url = cJSON_GetStringValue(image_url_obj);
            config_manager_set_image_url(url);
        }

        cJSON *ha_url_obj = cJSON_GetObjectItem(root, "ha_url");
        if (ha_url_obj && cJSON_IsString(ha_url_obj)) {
            const char *url = cJSON_GetStringValue(ha_url_obj);
            config_manager_set_ha_url(url);
        }

        cJSON *rotation_mode_obj = cJSON_GetObjectItem(root, "rotation_mode");
        if (rotation_mode_obj && cJSON_IsString(rotation_mode_obj)) {
            const char *mode_str = cJSON_GetStringValue(rotation_mode_obj);
            rotation_mode_t mode =
                (strcmp(mode_str, "url") == 0) ? ROTATION_MODE_URL : ROTATION_MODE_SDCARD;
            config_manager_set_rotation_mode(mode);
        }

        cJSON *save_dl_obj = cJSON_GetObjectItem(root, "save_downloaded_images");
        if (save_dl_obj && cJSON_IsBool(save_dl_obj)) {
            bool save_dl = cJSON_IsTrue(save_dl_obj);
            config_manager_set_save_downloaded_images(save_dl);
        }

        cJSON *display_orient_obj = cJSON_GetObjectItem(root, "display_orientation");
        if (display_orient_obj && cJSON_IsString(display_orient_obj)) {
            const char *orient_str = cJSON_GetStringValue(display_orient_obj);
            if (strcmp(orient_str, "portrait") == 0) {
                config_manager_set_display_orientation(DISPLAY_ORIENTATION_PORTRAIT);
            } else {
                config_manager_set_display_orientation(DISPLAY_ORIENTATION_LANDSCAPE);
            }
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

static esp_err_t albums_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System not ready");
        return ESP_OK;
    }

    if (req->method == HTTP_GET) {
        char **albums = NULL;
        int count = 0;
        if (album_manager_list_albums(&albums, &count) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to list albums");
            return ESP_FAIL;
        }

        cJSON *response = cJSON_CreateArray();
        for (int i = 0; i < count; i++) {
            cJSON *album_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(album_obj, "name", albums[i]);
            cJSON_AddBoolToObject(album_obj, "enabled", album_manager_is_album_enabled(albums[i]));
            cJSON_AddItemToArray(response, album_obj);
        }
        album_manager_free_album_list(albums, count);

        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        free(json_str);
        cJSON_Delete(response);
        return ESP_OK;
    } else if (req->method == HTTP_POST) {
        char buf[256];
        int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request");
            return ESP_FAIL;
        }
        buf[ret] = '\0';

        cJSON *root = cJSON_Parse(buf);
        if (!root) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            return ESP_FAIL;
        }

        cJSON *name_json = cJSON_GetObjectItem(root, "name");
        if (!name_json || !cJSON_IsString(name_json)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing album name");
            return ESP_FAIL;
        }

        const char *album_name = name_json->valuestring;
        esp_err_t err = album_manager_create_album(album_name);
        cJSON_Delete(root);

        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create album");
            return ESP_FAIL;
        }

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

static esp_err_t album_delete_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System not ready");
        return ESP_OK;
    }

    // Use query parameter since ESP-IDF httpd doesn't support wildcard URIs
    char query[256];
    char album_name[128];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing album name");
        return ESP_FAIL;
    }

    if (httpd_query_key_value(query, "name", album_name, sizeof(album_name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing album name parameter");
        return ESP_FAIL;
    }

    esp_err_t err = album_manager_delete_album(album_name);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete album");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t album_enabled_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System not ready");
        return ESP_OK;
    }

    // Get album name from query parameter
    char query[256];
    char album_name[128];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing album name");
        return ESP_FAIL;
    }

    if (httpd_query_key_value(query, "name", album_name, sizeof(album_name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing album name parameter");
        return ESP_FAIL;
    }

    // Get enabled status from JSON body
    char buf[256];
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *enabled_json = cJSON_GetObjectItem(root, "enabled");

    if (!enabled_json || !cJSON_IsBool(enabled_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing enabled field");
        return ESP_FAIL;
    }

    bool enabled = cJSON_IsTrue(enabled_json);

    esp_err_t err = album_manager_set_album_enabled(album_name, enabled);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to update album");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t album_images_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System not ready");
        return ESP_OK;
    }

    char query[256];
    char album_name[128] = "";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "album", album_name, sizeof(album_name));
    }

    if (strlen(album_name) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing album parameter");
        return ESP_FAIL;
    }

    char album_path[256];
    if (album_manager_get_album_path(album_name, album_path, sizeof(album_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid album");
        return ESP_FAIL;
    }

    DIR *dir = opendir(album_path);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open album directory");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateArray();
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            if (entry->d_name[0] == '.' && entry->d_name[1] == '_') {
                continue;
            }
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0)) {
                cJSON *image_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(image_obj, "name", entry->d_name);
                cJSON_AddStringToObject(image_obj, "album", album_name);
                cJSON_AddItemToArray(response, image_obj);
            }
        }
    }
    closedir(dir);

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t version_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "version", app_desc->version);
    cJSON_AddStringToObject(response, "project_name", app_desc->project_name);
    cJSON_AddStringToObject(response, "compile_time", app_desc->time);
    cJSON_AddStringToObject(response, "compile_date", app_desc->date);
    cJSON_AddStringToObject(response, "idf_version", app_desc->idf_ver);

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    ota_status_t status;
    ota_get_status(&status);

    cJSON *response = cJSON_CreateObject();

    // Add state as string
    const char *state_str = "idle";
    switch (status.state) {
    case OTA_STATE_IDLE:
        state_str = "idle";
        break;
    case OTA_STATE_CHECKING:
        state_str = "checking";
        break;
    case OTA_STATE_UPDATE_AVAILABLE:
        state_str = "update_available";
        break;
    case OTA_STATE_DOWNLOADING:
        state_str = "downloading";
        break;
    case OTA_STATE_INSTALLING:
        state_str = "installing";
        break;
    case OTA_STATE_SUCCESS:
        state_str = "success";
        break;
    case OTA_STATE_ERROR:
        state_str = "error";
        break;
    }

    cJSON_AddStringToObject(response, "state", state_str);
    cJSON_AddStringToObject(response, "current_version", status.current_version);
    cJSON_AddStringToObject(response, "latest_version", status.latest_version);
    cJSON_AddNumberToObject(response, "progress_percent", status.progress_percent);

    if (status.error_message[0] != '\0') {
        cJSON_AddStringToObject(response, "error_message", status.error_message);
    }

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t ota_check_handler(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        bool update_available = false;
        esp_err_t err = ota_check_for_update(&update_available, 30);

        cJSON *response = cJSON_CreateObject();
        if (err == ESP_OK) {
            cJSON_AddBoolToObject(response, "update_available", update_available);
            cJSON_AddStringToObject(response, "status", "success");
        } else {
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "message", "Failed to check for updates");
        }

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

static esp_err_t ota_update_handler(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        esp_err_t err = ota_start_update();

        cJSON *response = cJSON_CreateObject();
        if (err == ESP_OK) {
            cJSON_AddStringToObject(response, "status", "success");
            cJSON_AddStringToObject(response, "message", "OTA update started");
        } else if (err == ESP_ERR_INVALID_STATE) {
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "message",
                                    "No update available or update already in progress");
        } else {
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "message", "Failed to start OTA update");
        }

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

static esp_err_t keep_alive_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    // Reset sleep timer to keep device awake while webapp is actively being used
    power_manager_reset_sleep_timer();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");

    return ESP_OK;
}

static esp_err_t display_calibration_handler(httpd_req_t *req)
{
    const size_t calibration_bmp_size = (calibration_bmp_end - calibration_bmp_start);

    ESP_LOGI(TAG, "Displaying calibration pattern on e-paper");

    // Write embedded BMP to temporary file
    const char *temp_path = "/sdcard/.calibration.bmp";

    // Copy embedded data to SPIRAM buffer first to avoid cache coherency issues
    // when writing directly from flash memory to SD card
    uint8_t *bmp_buffer = (uint8_t *) heap_caps_malloc(calibration_bmp_size, MALLOC_CAP_SPIRAM);
    if (!bmp_buffer) {
        ESP_LOGE(TAG, "Failed to allocate SPIRAM buffer");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Out of memory\"}");
        return ESP_FAIL;
    }

    memcpy(bmp_buffer, calibration_bmp_start, calibration_bmp_size);

    FILE *f = fopen(temp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create temporary calibration file");
        free(bmp_buffer);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(
            req, "{\"status\":\"error\",\"message\":\"Failed to create temporary file\"}");
        return ESP_FAIL;
    }

    // Write from SPIRAM buffer
    size_t written = fwrite(bmp_buffer, 1, calibration_bmp_size, f);
    free(bmp_buffer);
    fclose(f);

    if (written != calibration_bmp_size) {
        ESP_LOGE(TAG, "Failed to write calibration BMP");
        unlink(temp_path);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(
            req, "{\"status\":\"error\",\"message\":\"Failed to write calibration data\"}");
        return ESP_FAIL;
    }

    // Display the calibration pattern
    esp_err_t ret = display_manager_show_image(temp_path);

    // Clean up temporary file
    unlink(temp_path);

    if (ret == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(
            req, "{\"status\":\"success\",\"message\":\"Calibration pattern displayed\"}");
        return ESP_OK;
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(
            req, "{\"status\":\"error\",\"message\":\"Failed to display calibration pattern\"}");
        return ESP_FAIL;
    }
}

static esp_err_t processing_settings_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        processing_settings_t settings;
        if (processing_settings_load(&settings) != ESP_OK) {
            processing_settings_get_defaults(&settings);
        }

        cJSON *response = cJSON_CreateObject();
        cJSON_AddNumberToObject(response, "exposure", settings.exposure);
        cJSON_AddNumberToObject(response, "saturation", settings.saturation);
        cJSON_AddStringToObject(response, "toneMode", settings.tone_mode);
        cJSON_AddNumberToObject(response, "contrast", settings.contrast);
        cJSON_AddNumberToObject(response, "strength", settings.strength);
        cJSON_AddNumberToObject(response, "shadowBoost", settings.shadow_boost);
        cJSON_AddNumberToObject(response, "highlightCompress", settings.highlight_compress);
        cJSON_AddNumberToObject(response, "midpoint", settings.midpoint);
        cJSON_AddStringToObject(response, "colorMethod", settings.color_method);
        cJSON_AddBoolToObject(response, "renderMeasured", settings.render_measured);
        cJSON_AddStringToObject(response, "processingMode", settings.processing_mode);

        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);

        free(json_str);
        cJSON_Delete(response);
        return ESP_OK;

    } else if (req->method == HTTP_POST) {
        char *buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
        if (!buf) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        int ret = httpd_req_recv(req, buf, req->content_len);
        if (ret <= 0) {
            heap_caps_free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        buf[ret] = '\0';

        cJSON *json = cJSON_Parse(buf);
        heap_caps_free(buf);

        if (!json) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            return ESP_FAIL;
        }

        processing_settings_t settings;
        processing_settings_get_defaults(&settings);

        cJSON *item;
        if ((item = cJSON_GetObjectItem(json, "exposure")) && cJSON_IsNumber(item)) {
            settings.exposure = (float) item->valuedouble;
        }
        if ((item = cJSON_GetObjectItem(json, "saturation")) && cJSON_IsNumber(item)) {
            settings.saturation = (float) item->valuedouble;
        }
        if ((item = cJSON_GetObjectItem(json, "toneMode")) && cJSON_IsString(item)) {
            strncpy(settings.tone_mode, item->valuestring, sizeof(settings.tone_mode) - 1);
        }
        if ((item = cJSON_GetObjectItem(json, "contrast")) && cJSON_IsNumber(item)) {
            settings.contrast = (float) item->valuedouble;
        }
        if ((item = cJSON_GetObjectItem(json, "strength")) && cJSON_IsNumber(item)) {
            settings.strength = (float) item->valuedouble;
        }
        if ((item = cJSON_GetObjectItem(json, "shadowBoost")) && cJSON_IsNumber(item)) {
            settings.shadow_boost = (float) item->valuedouble;
        }
        if ((item = cJSON_GetObjectItem(json, "highlightCompress")) && cJSON_IsNumber(item)) {
            settings.highlight_compress = (float) item->valuedouble;
        }
        if ((item = cJSON_GetObjectItem(json, "midpoint")) && cJSON_IsNumber(item)) {
            settings.midpoint = (float) item->valuedouble;
        }
        if ((item = cJSON_GetObjectItem(json, "colorMethod")) && cJSON_IsString(item)) {
            strncpy(settings.color_method, item->valuestring, sizeof(settings.color_method) - 1);
        }
        if ((item = cJSON_GetObjectItem(json, "renderMeasured")) && cJSON_IsBool(item)) {
            settings.render_measured = cJSON_IsTrue(item);
        }
        if ((item = cJSON_GetObjectItem(json, "processingMode")) && cJSON_IsString(item)) {
            strncpy(settings.processing_mode, item->valuestring,
                    sizeof(settings.processing_mode) - 1);
        }

        cJSON_Delete(json);

        esp_err_t err = processing_settings_save(&settings);
        if (err != ESP_OK) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":true}");
        return ESP_OK;

    } else if (req->method == HTTP_DELETE) {
        // Reset to firmware defaults
        processing_settings_t settings;
        processing_settings_get_defaults(&settings);

        // Save defaults to NVS
        esp_err_t err = processing_settings_save(&settings);
        if (err != ESP_OK) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        // Return the default values
        cJSON *response = cJSON_CreateObject();
        cJSON_AddNumberToObject(response, "exposure", settings.exposure);
        cJSON_AddNumberToObject(response, "saturation", settings.saturation);
        cJSON_AddStringToObject(response, "toneMode", settings.tone_mode);
        cJSON_AddNumberToObject(response, "contrast", settings.contrast);
        cJSON_AddNumberToObject(response, "strength", settings.strength);
        cJSON_AddNumberToObject(response, "shadowBoost", settings.shadow_boost);
        cJSON_AddNumberToObject(response, "highlightCompress", settings.highlight_compress);
        cJSON_AddNumberToObject(response, "midpoint", settings.midpoint);
        cJSON_AddStringToObject(response, "colorMethod", settings.color_method);
        cJSON_AddBoolToObject(response, "renderMeasured", settings.render_measured);
        cJSON_AddStringToObject(response, "processingMode", settings.processing_mode);

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

static esp_err_t color_palette_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        color_palette_t palette;
        if (color_palette_load(&palette) != ESP_OK) {
            color_palette_get_defaults(&palette);
        }

        cJSON *response = cJSON_CreateObject();

        cJSON *black = cJSON_CreateObject();
        cJSON_AddNumberToObject(black, "r", palette.black.r);
        cJSON_AddNumberToObject(black, "g", palette.black.g);
        cJSON_AddNumberToObject(black, "b", palette.black.b);
        cJSON_AddItemToObject(response, "black", black);

        cJSON *white = cJSON_CreateObject();
        cJSON_AddNumberToObject(white, "r", palette.white.r);
        cJSON_AddNumberToObject(white, "g", palette.white.g);
        cJSON_AddNumberToObject(white, "b", palette.white.b);
        cJSON_AddItemToObject(response, "white", white);

        cJSON *yellow = cJSON_CreateObject();
        cJSON_AddNumberToObject(yellow, "r", palette.yellow.r);
        cJSON_AddNumberToObject(yellow, "g", palette.yellow.g);
        cJSON_AddNumberToObject(yellow, "b", palette.yellow.b);
        cJSON_AddItemToObject(response, "yellow", yellow);

        cJSON *red = cJSON_CreateObject();
        cJSON_AddNumberToObject(red, "r", palette.red.r);
        cJSON_AddNumberToObject(red, "g", palette.red.g);
        cJSON_AddNumberToObject(red, "b", palette.red.b);
        cJSON_AddItemToObject(response, "red", red);

        cJSON *blue = cJSON_CreateObject();
        cJSON_AddNumberToObject(blue, "r", palette.blue.r);
        cJSON_AddNumberToObject(blue, "g", palette.blue.g);
        cJSON_AddNumberToObject(blue, "b", palette.blue.b);
        cJSON_AddItemToObject(response, "blue", blue);

        cJSON *green = cJSON_CreateObject();
        cJSON_AddNumberToObject(green, "r", palette.green.r);
        cJSON_AddNumberToObject(green, "g", palette.green.g);
        cJSON_AddNumberToObject(green, "b", palette.green.b);
        cJSON_AddItemToObject(response, "green", green);

        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);

        free(json_str);
        cJSON_Delete(response);
        return ESP_OK;

    } else if (req->method == HTTP_POST) {
        char *buf = malloc(req->content_len + 1);
        if (!buf) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        int ret = httpd_req_recv(req, buf, req->content_len);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        buf[ret] = '\0';

        cJSON *json = cJSON_Parse(buf);
        free(buf);

        if (!json) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            return ESP_FAIL;
        }

        color_palette_t palette;
        color_palette_get_defaults(&palette);

        cJSON *color;
        cJSON *component;

        if ((color = cJSON_GetObjectItem(json, "black"))) {
            if ((component = cJSON_GetObjectItem(color, "r")) && cJSON_IsNumber(component))
                palette.black.r = (uint8_t) component->valueint;
            if ((component = cJSON_GetObjectItem(color, "g")) && cJSON_IsNumber(component))
                palette.black.g = (uint8_t) component->valueint;
            if ((component = cJSON_GetObjectItem(color, "b")) && cJSON_IsNumber(component))
                palette.black.b = (uint8_t) component->valueint;
        }

        if ((color = cJSON_GetObjectItem(json, "white"))) {
            if ((component = cJSON_GetObjectItem(color, "r")) && cJSON_IsNumber(component))
                palette.white.r = (uint8_t) component->valueint;
            if ((component = cJSON_GetObjectItem(color, "g")) && cJSON_IsNumber(component))
                palette.white.g = (uint8_t) component->valueint;
            if ((component = cJSON_GetObjectItem(color, "b")) && cJSON_IsNumber(component))
                palette.white.b = (uint8_t) component->valueint;
        }

        if ((color = cJSON_GetObjectItem(json, "yellow"))) {
            if ((component = cJSON_GetObjectItem(color, "r")) && cJSON_IsNumber(component))
                palette.yellow.r = (uint8_t) component->valueint;
            if ((component = cJSON_GetObjectItem(color, "g")) && cJSON_IsNumber(component))
                palette.yellow.g = (uint8_t) component->valueint;
            if ((component = cJSON_GetObjectItem(color, "b")) && cJSON_IsNumber(component))
                palette.yellow.b = (uint8_t) component->valueint;
        }

        if ((color = cJSON_GetObjectItem(json, "red"))) {
            if ((component = cJSON_GetObjectItem(color, "r")) && cJSON_IsNumber(component))
                palette.red.r = (uint8_t) component->valueint;
            if ((component = cJSON_GetObjectItem(color, "g")) && cJSON_IsNumber(component))
                palette.red.g = (uint8_t) component->valueint;
            if ((component = cJSON_GetObjectItem(color, "b")) && cJSON_IsNumber(component))
                palette.red.b = (uint8_t) component->valueint;
        }

        if ((color = cJSON_GetObjectItem(json, "blue"))) {
            if ((component = cJSON_GetObjectItem(color, "r")) && cJSON_IsNumber(component))
                palette.blue.r = (uint8_t) component->valueint;
            if ((component = cJSON_GetObjectItem(color, "g")) && cJSON_IsNumber(component))
                palette.blue.g = (uint8_t) component->valueint;
            if ((component = cJSON_GetObjectItem(color, "b")) && cJSON_IsNumber(component))
                palette.blue.b = (uint8_t) component->valueint;
        }

        if ((color = cJSON_GetObjectItem(json, "green"))) {
            if ((component = cJSON_GetObjectItem(color, "r")) && cJSON_IsNumber(component))
                palette.green.r = (uint8_t) component->valueint;
            if ((component = cJSON_GetObjectItem(color, "g")) && cJSON_IsNumber(component))
                palette.green.g = (uint8_t) component->valueint;
            if ((component = cJSON_GetObjectItem(color, "b")) && cJSON_IsNumber(component))
                palette.green.b = (uint8_t) component->valueint;
        }

        cJSON_Delete(json);

        esp_err_t err = color_palette_save(&palette);
        if (err != ESP_OK) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        // Reload palette in image processor so subsequent uploads use the new calibration
        image_processor_reload_palette();

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":true}");
        return ESP_OK;
    } else if (req->method == HTTP_DELETE) {
        // Reset palette to defaults
        color_palette_t palette;
        color_palette_get_defaults(&palette);

        esp_err_t err = color_palette_save(&palette);
        if (err != ESP_OK) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        // Reload palette in image processor
        image_processor_reload_palette();

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":true}");
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

esp_err_t http_server_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 36;
    config.stack_size = 12288;       // Increased from 8192 to 12KB
    config.max_open_sockets = 10;    // Limit concurrent connections to prevent memory exhaustion
    config.lru_purge_enable = true;  // Enable LRU purging of connections

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

        httpd_uri_t measurement_sample_uri = {.uri = "/measurement_sample.jpg",
                                              .method = HTTP_GET,
                                              .handler = measurement_sample_handler,
                                              .user_ctx = NULL};
        httpd_register_uri_handler(server, &measurement_sample_uri);

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

        httpd_uri_t rotate_uri = {
            .uri = "/api/rotate", .method = HTTP_POST, .handler = rotate_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &rotate_uri);

        httpd_uri_t current_image_uri = {.uri = "/api/current_image",
                                         .method = HTTP_GET,
                                         .handler = current_image_handler,
                                         .user_ctx = NULL};
        httpd_register_uri_handler(server, &current_image_uri);

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

        httpd_uri_t version_uri = {.uri = "/api/version",
                                   .method = HTTP_GET,
                                   .handler = version_handler,
                                   .user_ctx = NULL};
        httpd_register_uri_handler(server, &version_uri);

        httpd_uri_t ota_status_uri = {.uri = "/api/ota/status",
                                      .method = HTTP_GET,
                                      .handler = ota_status_handler,
                                      .user_ctx = NULL};
        httpd_register_uri_handler(server, &ota_status_uri);

        httpd_uri_t ota_check_uri = {.uri = "/api/ota/check",
                                     .method = HTTP_POST,
                                     .handler = ota_check_handler,
                                     .user_ctx = NULL};
        httpd_register_uri_handler(server, &ota_check_uri);

        httpd_uri_t ota_update_uri = {.uri = "/api/ota/update",
                                      .method = HTTP_POST,
                                      .handler = ota_update_handler,
                                      .user_ctx = NULL};
        httpd_register_uri_handler(server, &ota_update_uri);

        httpd_uri_t albums_get_uri = {
            .uri = "/api/albums", .method = HTTP_GET, .handler = albums_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &albums_get_uri);

        httpd_uri_t albums_post_uri = {
            .uri = "/api/albums", .method = HTTP_POST, .handler = albums_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &albums_post_uri);

        httpd_uri_t album_delete_uri = {.uri = "/api/albums",
                                        .method = HTTP_DELETE,
                                        .handler = album_delete_handler,
                                        .user_ctx = NULL};
        httpd_register_uri_handler(server, &album_delete_uri);

        httpd_uri_t album_enabled_uri = {.uri = "/api/albums/enabled",
                                         .method = HTTP_PUT,
                                         .handler = album_enabled_handler,
                                         .user_ctx = NULL};
        httpd_register_uri_handler(server, &album_enabled_uri);

        httpd_uri_t images_uri = {.uri = "/api/images",
                                  .method = HTTP_GET,
                                  .handler = album_images_handler,
                                  .user_ctx = NULL};
        httpd_register_uri_handler(server, &images_uri);

        httpd_uri_t processing_settings_get_uri = {.uri = "/api/settings/processing",
                                                   .method = HTTP_GET,
                                                   .handler = processing_settings_handler,
                                                   .user_ctx = NULL};
        httpd_register_uri_handler(server, &processing_settings_get_uri);

        httpd_uri_t processing_settings_post_uri = {.uri = "/api/settings/processing",
                                                    .method = HTTP_POST,
                                                    .handler = processing_settings_handler,
                                                    .user_ctx = NULL};
        httpd_register_uri_handler(server, &processing_settings_post_uri);

        httpd_uri_t processing_settings_delete_uri = {.uri = "/api/settings/processing",
                                                      .method = HTTP_DELETE,
                                                      .handler = processing_settings_handler,
                                                      .user_ctx = NULL};
        httpd_register_uri_handler(server, &processing_settings_delete_uri);

        httpd_uri_t color_palette_get_uri = {.uri = "/api/settings/palette",
                                             .method = HTTP_GET,
                                             .handler = color_palette_handler,
                                             .user_ctx = NULL};
        httpd_register_uri_handler(server, &color_palette_get_uri);

        httpd_uri_t color_palette_post_uri = {.uri = "/api/settings/palette",
                                              .method = HTTP_POST,
                                              .handler = color_palette_handler,
                                              .user_ctx = NULL};
        httpd_register_uri_handler(server, &color_palette_post_uri);

        httpd_uri_t color_palette_delete_uri = {.uri = "/api/settings/palette",
                                                .method = HTTP_DELETE,
                                                .handler = color_palette_handler,
                                                .user_ctx = NULL};
        httpd_register_uri_handler(server, &color_palette_delete_uri);

        httpd_uri_t display_calibration_uri = {.uri = "/api/calibration/display",
                                               .method = HTTP_POST,
                                               .handler = display_calibration_handler,
                                               .user_ctx = NULL};
        httpd_register_uri_handler(server, &display_calibration_uri);

        httpd_uri_t keep_alive_uri = {.uri = "/api/keep_alive",
                                      .method = HTTP_POST,
                                      .handler = keep_alive_handler,
                                      .user_ctx = NULL};
        httpd_register_uri_handler(server, &keep_alive_uri);

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
