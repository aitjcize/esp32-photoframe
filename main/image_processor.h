#ifndef IMAGE_PROCESSOR_H
#define IMAGE_PROCESSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "display_manager.h"
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
 * @param pub What to publish on completion (current-image name, optional
 *            album snapshot and fallback; see display_publish_t); NULL for
 *            an anonymous buffer display
 * @return esp_err_t ESP_OK on success; ESP_ERR_NOT_FINISHED when displayed
 *         but the requested snapshot failed
 */
esp_err_t image_processor_process_to_display(const uint8_t *input_data, size_t input_size,
                                             image_format_t format,
                                             dither_algorithm_t dither_algorithm,
                                             const display_publish_t *pub);

/**
 * @brief Display a PNG file, processing it only when necessary
 *
 * A pre-processed PNG (native dimensions, every pixel a theoretical output
 * color) is validated and painted straight from the file in a single decode
 * with no RAM copy; anything else falls back to
 * image_processor_process_to_display. Preferred entry point for PNG display
 * requests. With release_source set, the file is unlinked as soon as an
 * in-RAM copy exists (for MemFS-backed sources that live in PSRAM).
 */
esp_err_t image_processor_process_or_display_png(const char *path,
                                                 dither_algorithm_t dither_algorithm,
                                                 const display_publish_t *pub, bool release_source);

esp_err_t image_processor_reload_palette(void);

/**
 * @brief Human-readable reason for the most recent processing failure
 *
 * Empty string when the last operation succeeded. Suitable for appending to
 * HTTP error responses.
 */
const char *image_processor_get_last_error(void);

image_format_t image_processor_detect_format(const char *input_path);

/**
 * @brief Detect image format from buffer data
 */
image_format_t image_processor_detect_format_buffer(const uint8_t *data, size_t size);

/**
 * @brief Read just the pixel dimensions of an encoded image (PNG/JPG) without
 * decoding pixel data. Used to classify portrait vs. landscape cheaply before
 * committing to a full decode.
 */
/**
 * @brief Same as image_processor_peek_dimensions(), but reads the leading
 * bytes of a file itself rather than requiring the caller to already have a
 * buffer - a header-only peek, not a full decode.
 */
esp_err_t image_processor_peek_file_dimensions(const char *path, image_format_t format, int *out_w,
                                               int *out_h);

esp_err_t image_processor_peek_dimensions(const uint8_t *data, size_t size, image_format_t format,
                                          int *out_width, int *out_height);

/**
 * @brief Compose two source images into one canvas at the board's native
 * display resolution and process it exactly like a normal single image
 * (cover-fit each half, then CDR + dither).
 *
 * Used to combine two mismatched-orientation Telegram photos (e.g. two
 * portrait shots on a landscape frame) into one image instead of ever
 * displaying one alone.
 *
 * @param stack_vertically false = side-by-side (for a landscape-mounted
 * frame receiving portrait photos), true = stacked top/bottom (for a
 * portrait-mounted frame receiving landscape photos).
 */
esp_err_t image_processor_compose_pair_to_rgb(const uint8_t *data_a, size_t size_a,
                                              image_format_t format_a, const uint8_t *data_b,
                                              size_t size_b, image_format_t format_b,
                                              bool stack_vertically,
                                              dither_algorithm_t dither_algorithm,
                                              image_process_rgb_result_t *result);

/**
 * @brief Overlays a caption bar (solid background + wrapped bitmap-font text)
 * across the bottom of an already-processed (dithered, palette-quantized)
 * RGB888 buffer. Uses the exact display palette so the result stays a valid
 * "processed" image. No-op if caption is NULL/empty.
 */
void image_processor_draw_caption(uint8_t *rgb_buffer, int width, int height, const char *caption);

/**
 * @brief Same as image_processor_draw_caption(), but reads an already
 * display-processed PNG file, overlays the caption, and re-writes it in
 * place (no re-dithering). No-op (returns ESP_OK) if caption is NULL/empty.
 */
esp_err_t image_processor_add_caption_to_file(const char *png_path, const char *caption);

/**
 * @brief Writes an already-processed RGB888 buffer to a PNG file. Thin
 * wrapper so callers outside this module (e.g. the Telegram orientation-pair
 * compositor) can persist a composed/captioned buffer without duplicating
 * libpng plumbing.
 */
esp_err_t image_processor_write_rgb_to_png(const uint8_t *rgb_buffer, int width, int height,
                                           const char *output_path);

/**
 * @brief Generates a small preview thumbnail from an already-processed PNG,
 * nearest-neighbor downsampled to fit within max_dimension on its longer
 * edge (aspect ratio preserved). Used for images that don't otherwise get a
 * thumbnail sidecar generated client-side (e.g. Telegram downloads).
 */
esp_err_t image_processor_make_thumbnail(const char *source_png_path, int max_dimension,
                                         const char *output_path);

#endif
