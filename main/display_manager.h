#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t display_manager_init(void);
esp_err_t display_manager_show_image(const char *filename);

esp_err_t display_manager_show_calibration(void);
esp_err_t display_manager_clear(void);
bool display_manager_is_busy(void);
void display_manager_rotate_from_storage(void);
const char *display_manager_get_current_image(void);
void display_manager_initialize_paint(void);

/**
 * @brief Display an RGB buffer directly on the e-paper display
 *
 * This function takes an already-processed RGB buffer (with colors matching
 * the 6-color palette) and displays it directly. This is more efficient for
 * SD-card-less systems where no file I/O is needed.
 *
 * @param rgb_buffer RGB888 buffer (3 bytes per pixel, already dithered to palette)
 * @param width Image width
 * @param height Image height
 * @return esp_err_t ESP_OK on success
 */
esp_err_t display_manager_show_rgb_buffer(const uint8_t *rgb_buffer, int width, int height);

/**
 * @brief Row-streaming variant of display_manager_show_rgb_buffer
 *
 * begin acquires the display and clears the buffer; push paints one RGB row
 * (rows must arrive top to bottom); end refreshes the panel (when show is
 * true) and releases the display. After a successful begin, end MUST be
 * called on every path. Used by the image processor to display panel-size
 * images without materializing a full RGB frame.
 *
 * end's filename (nullable) is the logical name recorded as the current
 * image (as display_manager_show_image does for file-backed displays) so
 * /api/current_image can resolve the image's thumbnail; the named file
 * itself need not exist. NULL marks an anonymous buffer display.
 */
esp_err_t display_manager_begin_rgb_stream(void);
esp_err_t display_manager_push_rgb_row(int y, const uint8_t *rgb_row, int width);
// Column variant for rotated streaming: paints pixels (x, 0..height-1). Used
// when rows are produced in processing-space order on a rotated orientation.
esp_err_t display_manager_push_rgb_column(int x, const uint8_t *rgb_col, int height);
esp_err_t display_manager_end_rgb_stream(bool show, const char *filename);

#endif
