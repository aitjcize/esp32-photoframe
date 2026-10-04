#ifndef JPEG_SIZE_CHECK_H
#define JPEG_SIZE_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file jpeg_size_check.h
 * @brief A check of the size that esp_jpeg reports for a decoded JPEG.
 *
 * esp_jpeg_get_image_info() and esp_jpeg_decode() work out the size of the decoded picture as
 * (height / scale) * (width / scale) * bytes_per_pixel in 32 bit, from the sides in the header of
 * the file. A header that says 40000 x 35792 pixels makes that 4 295 040 000 bytes, which wraps to
 * 72 704; the decoder's own test that the output buffer is large enough uses the same wrapped
 * number, and esp_jpeg_decode() then writes the picture at its real size far past that buffer. The
 * sides come from a file that can originate outside the device (an upload, a download), so the
 * size is worked out again here, in 64 bit, before a buffer is allocated.
 */

/**
 * @brief Whether the size esp_jpeg reported is the size the picture's sides imply.
 * @param width, height The sides as the header says (what esp_jpeg_get_image_info() returns in
 *        width and height, whatever the scale).
 * @param shift The scale as a number of halvings (JPEG_IMAGE_SCALE_0 = 0 ... 1_8 = 3).
 * @param output_len The size esp_jpeg reported for 3 bytes a pixel (RGB888).
 */
static inline bool jpeg_output_size_ok(int width, int height, int shift, size_t output_len)
{
    if (width < 1 || height < 1 || shift < 0 || shift > 3) {
        return false;
    }
    uint64_t expected =
        (uint64_t) ((unsigned) height >> shift) * (uint64_t) ((unsigned) width >> shift) * 3;
    return expected > 0 && expected == (uint64_t) output_len;
}

#endif
