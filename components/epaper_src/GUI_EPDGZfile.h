// filename: GUI_EPDGZfile.h
#ifndef __GUI_EPDGZFILE_H
#define __GUI_EPDGZFILE_H

#include <stddef.h>
#include <stdint.h>

#include "GUI_Paint.h"

/**
 * @brief Read EPDGZ file and display it on the e-paper display
 *
 * Reads a gzip-compressed 4-bit-per-pixel raw e-paper image file.
 * Each byte contains two pixels (high nibble = first pixel, low nibble = second).
 * Color indices: 0=Black, 1=White, 2=Yellow, 3=Red, 5=Blue, 6=Green.
 *
 * @param path Path to the EPDGZ file
 * @return 0 on success, non-zero on error
 */
int GUI_ReadEPDGZ(const char *path);

/**
 * @brief Called with each packed row of an EPDGZ image, top to bottom.
 * @return 0 to continue, non-zero to stop with an error
 */
typedef int (*GUI_EPDGZRowFn)(void *ctx, int y, const uint8_t *row);

/**
 * @brief Stream an EPDGZ file row by row without holding the image in RAM
 *
 * @param path Path to the EPDGZ file
 * @param row_bytes Bytes per packed row: (width + 1) / 2
 * @param height Number of rows to deliver
 * @return 0 on success, non-zero on error (short file, corrupt data, or the
 *         callback stopped)
 */
int GUI_ReadEPDGZRows(const char *path, size_t row_bytes, int height, GUI_EPDGZRowFn fn, void *ctx);

#endif
