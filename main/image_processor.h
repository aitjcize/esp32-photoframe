#ifndef IMAGE_PROCESSOR_H
#define IMAGE_PROCESSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    DITHER_FLOYD_STEINBERG,
    DITHER_STUCKI,
    DITHER_BURKES,
    DITHER_SIERRA
} dither_algorithm_t;

typedef enum {
    IMAGE_FORMAT_UNKNOWN,
    IMAGE_FORMAT_PNG,
    IMAGE_FORMAT_BMP,
    IMAGE_FORMAT_JPG
} image_format_t;

/**
 * @brief Result structure for raw RGB buffer output (no PNG encoding)
 */
typedef struct {
    uint8_t *rgb_data;  // RGB888 buffer (caller must free with heap_caps_free)
    size_t rgb_size;    // Size of RGB data in bytes (width * height * 3)
    int width;          // Output image width
    int height;         // Output image height
} image_process_rgb_result_t;

esp_err_t image_processor_init(void);

/**
 * @brief Process image from file to file (legacy interface)
 *
 * This function reads from input_path, processes the image, and writes to output_path.
 * For SD-card systems, this is the preferred interface.
 */
esp_err_t image_processor_process(const char *input_path, const char *output_path,
                                  dither_algorithm_t dither_algorithm);

/**
 * @brief Process image from memory buffer to raw RGB buffer (no PNG encoding)
 *
 * This function takes raw image data (PNG or JPG), processes it, and returns
 * the processed RGB buffer directly. This is more efficient for SD-card-less
 * systems where the image can be displayed directly without PNG encode/decode.
 * The caller is responsible for freeing result->rgb_data with heap_caps_free().
 *
 * @param input_data Raw image data (PNG or JPG format)
 * @param input_size Size of input data in bytes
 * @param format Image format of input data
 * @param dither_algorithm Dithering algorithm to use
 * @param result Output structure containing processed RGB data
 * @return esp_err_t ESP_OK on success
 */
esp_err_t image_processor_process_to_rgb(const uint8_t *input_data, size_t input_size,
                                         image_format_t format, dither_algorithm_t dither_algorithm,
                                         image_process_rgb_result_t *result);

esp_err_t image_processor_reload_palette(void);

bool image_processor_is_processed(const char *input_path);

/**
 * @brief Check if buffer contains a pre-processed image
 */
bool image_processor_is_processed_buffer(const uint8_t *data, size_t size);

image_format_t image_processor_detect_format(const char *input_path);

/**
 * @brief Detect image format from buffer data
 */
image_format_t image_processor_detect_format_buffer(const uint8_t *data, size_t size);

#endif
