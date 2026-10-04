#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Size of the frame tjpgd (esp_jpeg) will actually decode. jd_prepare takes
// its width and height from the last SOF0 before SOS, while
// esp_jpeg_get_image_info stops at the first one, so a file with two SOF0
// headers can report a small picture and decode a huge one. This walks the
// header the way jd_prepare does: SOI, then marker segments up to SOS,
// unknown segments skipped, every other SOFn refused (tjpgd is baseline
// only). False on a truncated or malformed header.
//
// Kept free of ESP-IDF headers so it is host-tested.
bool jpeg_header_frame_size(const uint8_t *data, size_t size, int *width, int *height);
