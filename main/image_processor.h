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
    IMAGE_FORMAT_JPG,
    IMAGE_FORMAT_EPD_GZ
} image_format_t;

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
 * @brief Process image from memory buffer and show it on the display
 *
 * This function takes raw image data (PNG or JPG), processes it, and streams
 * the result row by row straight into the display buffer, then refreshes the
 * panel. The full-resolution processed image is never materialized in RAM.
 *
 * @param input_data Raw image data (PNG or JPG format)
 * @param input_size Size of input data in bytes
 * @param format Image format of input data
 * @param dither_algorithm Dithering algorithm to use
 * @param display_name Logical name recorded as the current image so its
 *                     thumbnail resolves (see display_manager_end_rgb_stream);
 *                     NULL for an anonymous buffer display
 * @return esp_err_t ESP_OK on success
 */
esp_err_t image_processor_process_to_display(const uint8_t *input_data, size_t input_size,
                                             image_format_t format,
                                             dither_algorithm_t dither_algorithm,
                                             const char *display_name);

/**
 * @brief Display a PNG file, processing it only when necessary
 *
 * A pre-processed PNG (native dimensions, every pixel a theoretical output
 * color) is validated and painted straight from the file in a single decode
 * with no RAM copy; anything else falls back to
 * image_processor_process_to_display. Preferred entry point for the direct
 * display endpoint's PNG uploads.
 */
esp_err_t image_processor_process_or_display_png(const char *path,
                                                 dither_algorithm_t dither_algorithm,
                                                 const char *display_name);

esp_err_t image_processor_reload_palette(void);

/**
 * @brief Human-readable reason for the most recent processing failure
 *
 * Empty string when the last operation succeeded. Suitable for appending to
 * HTTP error responses.
 */
const char *image_processor_get_last_error(void);

bool image_processor_is_processed(const char *input_path);

image_format_t image_processor_detect_format(const char *input_path);

/**
 * @brief Detect image format from buffer data
 */
image_format_t image_processor_detect_format_buffer(const uint8_t *data, size_t size);

#endif
