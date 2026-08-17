// Fake display_manager for host tests: the streaming API paints into a
// malloc'd RGB frame instead of the e-paper framebuffer.
#include "fake_display_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_hal.h"
#include "esp_err.h"

static uint8_t *frame = NULL;
static int frame_w = 0;
static int frame_h = 0;
static bool streaming = false;
static bool shown = false;
static int begin_count = 0;
static char pub_display_name[512];
static char pub_save_path[512];
static char pub_fallback_name[512];

void fake_display_reset(void)
{
    free(frame);
    frame = NULL;
    frame_w = 0;
    frame_h = 0;
    streaming = false;
    shown = false;
    begin_count = 0;
    pub_display_name[0] = '\0';
    pub_save_path[0] = '\0';
    pub_fallback_name[0] = '\0';
}

int fake_display_frame_width(void)
{
    return frame_w;
}

int fake_display_frame_height(void)
{
    return frame_h;
}

const uint8_t *fake_display_frame(void)
{
    return frame;
}

bool fake_display_was_shown(void)
{
    return shown;
}

int fake_display_begin_count(void)
{
    return begin_count;
}

const char *fake_display_pub_display_name(void)
{
    return pub_display_name;
}

const char *fake_display_pub_save_path(void)
{
    return pub_save_path;
}

const char *fake_display_pub_fallback_name(void)
{
    return pub_fallback_name;
}

esp_err_t display_manager_begin_rgb_stream(void)
{
    if (streaming)
        return ESP_ERR_INVALID_STATE;
    free(frame);
    frame_w = test_board_display_width;
    frame_h = test_board_display_height;
    frame = malloc((size_t) frame_w * frame_h * 3);
    if (!frame)
        return ESP_ERR_NO_MEM;
    // The real begin clears the paint buffer to white
    memset(frame, 0xFF, (size_t) frame_w * frame_h * 3);
    streaming = true;
    shown = false;
    begin_count++;
    return ESP_OK;
}

esp_err_t display_manager_push_rgb_row(int y, const uint8_t *rgb_row, int width)
{
    if (!streaming || y < 0 || y >= frame_h || width != frame_w)
        return ESP_ERR_INVALID_ARG;
    memcpy(frame + (size_t) y * frame_w * 3, rgb_row, (size_t) width * 3);
    return ESP_OK;
}

esp_err_t display_manager_push_rgb_column(int x, const uint8_t *rgb_col, int height)
{
    if (!streaming || x < 0 || x >= frame_w || height != frame_h)
        return ESP_ERR_INVALID_ARG;
    for (int y = 0; y < height; y++)
        memcpy(frame + ((size_t) y * frame_w + x) * 3, rgb_col + (size_t) y * 3, 3);
    return ESP_OK;
}

esp_err_t display_manager_end_rgb_stream(bool show, const display_publish_t *pub)
{
    if (!streaming)
        return ESP_ERR_INVALID_STATE;
    streaming = false;
    shown = show;
    snprintf(pub_display_name, sizeof(pub_display_name), "%s",
             pub && pub->display_name ? pub->display_name : "");
    snprintf(pub_save_path, sizeof(pub_save_path), "%s",
             pub && pub->save_path ? pub->save_path : "");
    snprintf(pub_fallback_name, sizeof(pub_fallback_name), "%s",
             pub && pub->fallback_name ? pub->fallback_name : "");
    return ESP_OK;
}
