#pragma once

#include <stddef.h>
#include <stdint.h>

// Row copies for drivers sending a packed 4-bit frame buffer (2 px/byte, the
// first pixel in the high nibble) to a panel. They copy src to dst, which must
// not overlap, so a driver can transform each row as it fills its DMA buffer
// instead of rewriting the frame buffer. Kept free of ESP-IDF headers so they
// are host-tested.

// Copy n bytes in reverse pixel order: the last pixel first. Applied to the
// whole row this mirrors it left to right. Applied to the bytes [n - o - k,
// n - o) of a row it gives bytes [o, o + k) of the mirrored row, for drivers
// that send a row in pieces.
void epaper_row_reverse_pixels(uint8_t *dst, const uint8_t *src, size_t n);

// Copy n bytes (even) with the 16-bit words -- 4 pixels each -- in reverse
// order and each word's two bytes kept as they are.
void epaper_row_reverse_words(uint8_t *dst, const uint8_t *src, size_t n);

// Copy n bytes (even) with each 16-bit word in place but its 4 pixels
// reversed. The same as epaper_row_reverse_pixels followed by
// epaper_row_reverse_words, in one pass.
void epaper_row_reverse_pixels_in_words(uint8_t *dst, const uint8_t *src, size_t n);
