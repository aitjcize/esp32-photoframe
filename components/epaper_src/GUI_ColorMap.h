// filename: GUI_ColorMap.h
#ifndef __GUI_COLORMAP_H
#define __GUI_COLORMAP_H

#include <stdint.h>

#include "GUI_Paint.h"

/**
 * Shared RGB -> 4-bit pixel mappers for the RGB decode paths (PNG, BMP,
 * raw RGB buffer). Paint_SetPixel packs the returned value directly into
 * the framebuffer nibble, whose meaning depends on the panel: an ink-color
 * index on Spectra 6 panels, a linear intensity (0=black..15=white) on
 * grayscale (GC16/IT8951) panels. The caller picks the mapper matching
 * BOARD_HAL_DISPLAY_TYPE.
 */
typedef UBYTE (*GUI_RGBMapFn)(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Map an RGB pixel to a Spectra 6-color palette index.
 *
 * Exact match against the theoretical palette colors; unknown colors fall
 * back to white (index 1).
 */
static inline UBYTE GUI_RGBToSpectra6(uint8_t r, uint8_t g, uint8_t b)
{
    if (r == 0 && g == 0 && b == 0) {
        return 0;  // Black
    } else if (r == 255 && g == 255 && b == 255) {
        return 1;  // White
    } else if (r == 255 && g == 255 && b == 0) {
        return 2;  // Yellow
    } else if (r == 255 && g == 0 && b == 0) {
        return 3;  // Red
    } else if (r == 0 && g == 0 && b == 255) {
        return 5;  // Blue
    } else if (r == 0 && g == 255 && b == 0) {
        return 6;  // Green
    }
    return 1;  // Default to white for unknown colors
}

/**
 * @brief Map an RGB pixel to a GC16 gray level (0=black..15=white).
 *
 * Images converted by epaper-image-convert are dithered to the theoretical
 * 16-level ramp (neutral grays, value = round(level * 255 / 15)), and
 * rounding the luminance back recovers the level exactly — this mirrors
 * rgbToPaletteIndex() on the converter side. Input that never went through
 * the converter (e.g. a plain black-and-white PNG uploaded directly) also
 * lands on the nearest gray level instead of collapsing to black or white.
 */
static inline UBYTE GUI_RGBToGray16(uint8_t r, uint8_t g, uint8_t b)
{
    // Rec.601 integer luma; identity for neutral grays (r == g == b).
    uint32_t y = (299u * r + 587u * g + 114u * b) / 1000u;
    return (UBYTE) ((y * 15u + 127u) / 255u);
}

#endif
