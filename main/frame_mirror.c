#include "frame_mirror.h"

#include <stddef.h>

static inline uint8_t get_px(const uint8_t *row, int x)
{
    return (x % 2 == 0) ? (uint8_t) (row[x / 2] >> 4) : (uint8_t) (row[x / 2] & 0x0F);
}

static inline void set_px(uint8_t *row, int x, uint8_t v)
{
    if (x % 2 == 0) {
        row[x / 2] = (uint8_t) ((row[x / 2] & 0x0F) | (v << 4));
    } else {
        row[x / 2] = (uint8_t) ((row[x / 2] & 0xF0) | (v & 0x0F));
    }
}

void frame_mirror_x(uint8_t *buf, int width, int height)
{
    if (!buf || width < 2 || height < 1) {
        return;
    }
    const size_t row_bytes = (size_t) (width + 1) / 2;
    for (int y = 0; y < height; y++) {
        uint8_t *row = buf + (size_t) y * row_bytes;
        if (width % 2 == 0) {
            // Even width: reverse the bytes and swap each byte's two pixels
            for (size_t i = 0, j = row_bytes - 1; i < j; i++, j--) {
                uint8_t a = row[i];
                row[i] = (uint8_t) ((row[j] << 4) | (row[j] >> 4));
                row[j] = (uint8_t) ((a << 4) | (a >> 4));
            }
            if (row_bytes % 2 == 1) {
                uint8_t m = row[row_bytes / 2];
                row[row_bytes / 2] = (uint8_t) ((m << 4) | (m >> 4));
            }
        } else {
            // Odd width: the last byte's low nibble is padding, so swap pixel
            // by pixel
            for (int a = 0, b = width - 1; a < b; a++, b--) {
                uint8_t pa = get_px(row, a);
                set_px(row, a, get_px(row, b));
                set_px(row, b, pa);
            }
        }
    }
}
