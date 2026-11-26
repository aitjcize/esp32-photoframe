#include "epaper_row.h"

static inline uint8_t swap_nibbles(uint8_t b)
{
    return (uint8_t) ((b << 4) | (b >> 4));
}

void epaper_row_reverse_pixels(uint8_t *dst, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        dst[i] = swap_nibbles(src[n - 1 - i]);
    }
}

void epaper_row_reverse_words(uint8_t *dst, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i + 1 < n; i += 2) {
        dst[i] = src[n - 2 - i];
        dst[i + 1] = src[n - 1 - i];
    }
}

void epaper_row_reverse_pixels_in_words(uint8_t *dst, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i + 1 < n; i += 2) {
        dst[i] = swap_nibbles(src[i + 1]);
        dst[i + 1] = swap_nibbles(src[i]);
    }
}
