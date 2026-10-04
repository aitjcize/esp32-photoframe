#include "jpeg_header.h"

static unsigned be16(const uint8_t *p)
{
    return ((unsigned) p[0] << 8) | p[1];
}

bool jpeg_header_frame_size(const uint8_t *data, size_t size, int *width, int *height)
{
    if (!data || size < 2 || data[0] != 0xFF || data[1] != 0xD8) {
        return false;
    }
    int w = 0, h = 0;
    size_t ofs = 2;
    for (;;) {
        // tjpgd tolerates one unstuffed 0xFF in front of a marker
        if (size - ofs >= 2 && data[ofs] == 0xFF && data[ofs + 1] == 0xFF) {
            ofs++;
        }
        if (size - ofs < 4 || data[ofs] != 0xFF) {
            return false;
        }
        uint8_t marker = data[ofs + 1];
        size_t len = be16(data + ofs + 2);
        if (len <= 2) {
            return false;
        }
        len -= 2;
        const uint8_t *seg = data + ofs + 4;
        if (size - ofs - 4 < len) {
            return false;
        }
        ofs += 4 + len;

        if (marker == 0xC0) {
            // SOF0: the last one before SOS wins, as in jd_prepare
            if (len < 6) {
                return false;
            }
            h = (int) be16(seg + 1);
            w = (int) be16(seg + 3);
        } else if (marker == 0xDA) {
            // SOS
            if (w == 0 || h == 0) {
                return false;
            }
            *width = w;
            *height = h;
            return true;
        } else if ((marker >= 0xC1 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
                    marker != 0xCC) ||
                   marker == 0xD9) {
            // SOF1-SOF15 (progressive, lossless, ...) or EOI: tjpgd refuses these
            return false;
        }
    }
}
