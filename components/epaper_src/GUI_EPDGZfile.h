// filename: GUI_EPDGZfile.h
#ifndef __GUI_EPDGZFILE_H
#define __GUI_EPDGZFILE_H

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

#endif
