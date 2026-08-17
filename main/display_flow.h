#ifndef DISPLAY_FLOW_H
#define DISPLAY_FLOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "image_processor.h"

// Shared building blocks for the "get an image onto the panel" flows (the
// direct-display endpoint and the URL fetch). They encode the .current.*
// lifecycle invariants in one place:
//   - the display shows the newest image before any old file is touched, so
//     a failure never breaks /api/current_image
//   - the device cannot encode JPEG, so the displayed ORIGINAL is what gets
//     retired into the .current.* slot for its format and served back
//   - MemFS-backed /storage retains nothing full-size: the panel keeps the
//     image without power, and the file would occupy the PSRAM the next
//     upload needs

/**
 * @brief Read a whole file into a PSRAM buffer (caller frees)
 */
esp_err_t display_flow_read_file(const char *path, uint8_t **out_buf, size_t *out_size);

/**
 * @brief Stream a PNG/JPG source file to the display
 *
 * PNG uses the fused validate-or-process single-decode path straight from
 * the file; JPG is read into PSRAM and processed from there. With
 * release_source set (MemFS-backed sources), the file is unlinked as soon
 * as an in-RAM copy exists. The source file is not otherwise disposed of --
 * on success the caller retires it (see display_flow_retire_source).
 *
 * @return ESP_OK, ESP_ERR_NOT_FINISHED (displayed but the pub->save_path
 *         snapshot failed), or an error
 */
esp_err_t display_flow_stream_file(const char *path, image_format_t format,
                                   dither_algorithm_t algorithm, const display_publish_t *pub,
                                   bool release_source);

/**
 * @brief Move a display-ready BMP or EPDGZ into its .current.* slot
 *
 * On success returns the path to display; on failure unlinks the source and
 * returns NULL.
 */
const char *display_flow_stage_file(const char *source_path, image_format_t format);

/**
 * @brief Retire a displayed original into the .current.* scheme
 *
 * Call only after the panel shows the image. A JPG original becomes the
 * .current.jpg thumbnail (unless a fresh thumbnail for this image already
 * claims that slot); a PNG original is kept full-size as .current.png for
 * the thumbnail fallback. Stale siblings from earlier displays are dropped.
 * On MemFS nothing is retained. source_path may be NULL when the source was
 * already consumed.
 *
 * @param fresh_thumbnail a thumbnail belonging to THIS image occupies (or is
 *        about to occupy) the .current.jpg slot
 */
void display_flow_retire_source(const char *source_path, image_format_t format,
                                bool fresh_thumbnail);

/**
 * @brief Open the file /api/current_image should serve
 *
 * Reads the current-image link and opens the servable file: the .jpg
 * thumbnail sibling of the displayed name when it exists, otherwise the
 * displayed original with its native content type. Raw packed .epdgz is
 * never served. An open stream (rather than a path) is returned so a
 * concurrent display update cannot swap the file between selection and
 * read.
 *
 * @return open file (caller closes) with *out_content_type set, or NULL
 *         when nothing servable exists
 */
FILE *display_flow_open_current(const char **out_content_type);

/**
 * @brief Drop stale .current.* image files after a successful file display
 *
 * Unlinks .current.{png,bmp,epdgz} except keep_path (NULL keeps none), and
 * the .current.jpg thumbnail unless keep_thumbnail. On MemFS nothing is kept
 * regardless of keep_path.
 */
void display_flow_drop_stale_current(const char *keep_path, bool keep_thumbnail);

#endif
