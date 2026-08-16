// filename: GUI_RawBuffer.c
#include "GUI_RawBuffer.h"

#include <esp_log.h>

#include "GUI_ColorMap.h"

static const char *TAG = "GUI_RawBuffer";

static UBYTE display_rgb_buffer_mapped(const uint8_t *rgb_buffer, int width, int height,
                                       UWORD Xstart, UWORD Ystart, GUI_RGBMapFn map_rgb)
{
    if (!rgb_buffer) {
        ESP_LOGE(TAG, "NULL rgb_buffer");
        return 1;
    }

    ESP_LOGI(TAG, "Displaying RGB buffer: %dx%d at (%d,%d)", width, height, Xstart, Ystart);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if ((Xstart + x) >= Paint.Width || (Ystart + y) >= Paint.Height) {
                continue;
            }

            int offset = (y * width + x) * 3;
            uint8_t r = rgb_buffer[offset + 0];
            uint8_t g = rgb_buffer[offset + 1];
            uint8_t b = rgb_buffer[offset + 2];

            // The buffer should already be dithered to palette colors
            Paint_SetPixel(Xstart + x, Ystart + y, map_rgb(r, g, b));
        }
    }

    ESP_LOGI(TAG, "RGB buffer displayed successfully");
    return 0;
}

UBYTE GUI_DisplayRGBBuffer_6Color(const uint8_t *rgb_buffer, int width, int height, UWORD Xstart,
                                  UWORD Ystart)
{
    return display_rgb_buffer_mapped(rgb_buffer, width, height, Xstart, Ystart, GUI_RGBToSpectra6);
}

UBYTE GUI_DisplayRGBBuffer_Gray16(const uint8_t *rgb_buffer, int width, int height, UWORD Xstart,
                                  UWORD Ystart)
{
    return display_rgb_buffer_mapped(rgb_buffer, width, height, Xstart, Ystart, GUI_RGBToGray16);
}
