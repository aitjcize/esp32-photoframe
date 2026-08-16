// filename: GUI_PNGfile.h
#ifndef __GUI_PNGFILE_H
#define __GUI_PNGFILE_H

#include "GUI_Paint.h"

/**
 * @brief Read PNG file and display it on a Spectra 6-color e-paper display
 *
 * @param path Path to the PNG file
 * @param Xstart Starting X coordinate
 * @param Ystart Starting Y coordinate
 * @return 0 on success, 1 on error
 */
UBYTE GUI_ReadPng_RGB_6Color(const char *path, UWORD Xstart, UWORD Ystart);

/**
 * @brief Read PNG file and display it on a grayscale (GC16) e-paper display
 *
 * Pixels are mapped by luminance to gray levels 0..15 instead of Spectra
 * palette indices, so both converter output and plain black-and-white PNGs
 * render correctly.
 *
 * @param path Path to the PNG file
 * @param Xstart Starting X coordinate
 * @param Ystart Starting Y coordinate
 * @return 0 on success, 1 on error
 */
UBYTE GUI_ReadPng_Gray16(const char *path, UWORD Xstart, UWORD Ystart);

#endif
