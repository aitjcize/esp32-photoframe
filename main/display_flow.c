#include "display_flow.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "storage.h"

static const char *TAG = "display_flow";

esp_err_t display_flow_read_file(const char *path, uint8_t **out_buf, size_t *out_size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return ESP_FAIL;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        fclose(fp);
        return ESP_FAIL;
    }

    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    size_t read_bytes = fread(buf, 1, size, fp);
    fclose(fp);
    if (read_bytes != (size_t) size) {
        heap_caps_free(buf);
        return ESP_FAIL;
    }

    *out_buf = buf;
    *out_size = (size_t) size;
    return ESP_OK;
}

esp_err_t display_flow_stream_file(const char *path, image_format_t format,
                                   dither_algorithm_t algorithm, const display_publish_t *pub,
                                   bool release_source)
{
    if (format == IMAGE_FORMAT_PNG) {
        // File-backed fused path: no RAM copy of the source
        return image_processor_process_or_display_png(path, algorithm, pub, release_source);
    }

    uint8_t *buf = NULL;
    size_t size = 0;
    esp_err_t err = display_flow_read_file(path, &buf, &size);
    if (err != ESP_OK) {
        return err;
    }
    if (release_source) {
        // MemFS-backed source lives in PSRAM; drop the file now that the
        // compressed copy exists
        unlink(path);
    }
    err = image_processor_process_to_display(buf, size, format, algorithm, pub);
    heap_caps_free(buf);
    return err;
}

const char *display_flow_stage_file(const char *source_path, image_format_t format)
{
    const char *slot = (format == IMAGE_FORMAT_EPD_GZ) ? CURRENT_EPD_PATH : CURRENT_BMP_PATH;
    unlink(slot);
    if (rename(source_path, slot) != 0) {
        ESP_LOGE(TAG, "Failed to move %s into %s", source_path, slot);
        unlink(source_path);
        return NULL;
    }
    return slot;
}

void display_flow_retire_source(const char *source_path, image_format_t format,
                                bool fresh_thumbnail)
{
    if (storage_has_persistent_storage()) {
        if (format == IMAGE_FORMAT_JPG) {
            unlink(CURRENT_PNG_PATH);
            if (fresh_thumbnail) {
                // A thumbnail for this image claims the .current.jpg slot
                if (source_path) {
                    unlink(source_path);
                }
            } else if (source_path) {
                unlink(CURRENT_JPG_PATH);
                if (rename(source_path, CURRENT_JPG_PATH) != 0) {
                    // current_image will 404 rather than lie; the panel
                    // itself displayed fine
                    ESP_LOGW(TAG, "Failed to keep original JPEG as thumbnail");
                    unlink(source_path);
                }
            }
        } else {
            unlink(CURRENT_PNG_PATH);
            if (source_path && rename(source_path, CURRENT_PNG_PATH) != 0) {
                ESP_LOGW(TAG, "Failed to keep original PNG for current image");
                unlink(source_path);
            }
            if (!fresh_thumbnail) {
                unlink(CURRENT_JPG_PATH);  // stale thumbnail from an earlier display
            }
        }
    } else {
        // MemFS-backed /storage retains nothing full-size
        if (source_path) {
            unlink(source_path);
        }
        unlink(CURRENT_PNG_PATH);
        if (!fresh_thumbnail) {
            unlink(CURRENT_JPG_PATH);
        }
    }
    unlink(CURRENT_BMP_PATH);
    unlink(CURRENT_EPD_PATH);
}

void display_flow_drop_stale_current(const char *keep_path, bool keep_thumbnail)
{
    bool keep = storage_has_persistent_storage() && keep_path != NULL;
    if (!keep || strcmp(keep_path, CURRENT_PNG_PATH) != 0) {
        unlink(CURRENT_PNG_PATH);
    }
    if (!keep || strcmp(keep_path, CURRENT_BMP_PATH) != 0) {
        unlink(CURRENT_BMP_PATH);
    }
    if (!keep || strcmp(keep_path, CURRENT_EPD_PATH) != 0) {
        unlink(CURRENT_EPD_PATH);
    }
    if (!keep_thumbnail) {
        unlink(CURRENT_JPG_PATH);
    }
}
