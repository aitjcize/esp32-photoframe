#include "image_processor.h"

#include <limits.h>
#include <math.h>
#include <png.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "board_hal.h"
#include "color_palette.h"
#include "config_manager.h"
#include "display_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "processing_settings.h"

static const char *TAG = "image_processor";

// Human-readable reason for the most recent processing failure, surfaced in
// HTTP error responses
static char last_error_msg[96];

static void set_last_error(const char *msg)
{
    strncpy(last_error_msg, msg, sizeof(last_error_msg) - 1);
    last_error_msg[sizeof(last_error_msg) - 1] = '\0';
}

const char *image_processor_get_last_error(void)
{
    return last_error_msg;
}

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

typedef struct {
    int dx;
    int dy;
    int numerator;
    int denominator;
} error_diffusion_t;

// Theoretical palette - used for BMP output (firmware compatibility)
static const rgb_t palette[7] = {
    {0, 0, 0},        // Black
    {255, 255, 255},  // White
    {255, 255, 0},    // Yellow
    {255, 0, 0},      // Red
    {0, 0, 0},        // Reserved
    {0, 0, 255},      // Blue
    {0, 255, 0}       // Green
};

// Measured palette - loaded from config or defaults via color_palette module
static rgb_t palette_measured[7];

// GC16 grayscale palettes: the framebuffer nibble i is a linear intensity.
// Theoretical is the device output ramp (value = round(i * 255 / 15) = i * 17),
// mirroring epaper-image-convert; measured is the calibrated perceived ramp
// used for color matching and error diffusion.
#define GRAY(v) {(v), (v), (v)}
static const rgb_t gray_theoretical[16] = {
    GRAY(0),   GRAY(17),  GRAY(34),  GRAY(51),  GRAY(68),  GRAY(85),  GRAY(102), GRAY(119),
    GRAY(136), GRAY(153), GRAY(170), GRAY(187), GRAY(204), GRAY(221), GRAY(238), GRAY(255)};
#undef GRAY
static rgb_t gray_measured[16];

static bool board_is_grayscale(void)
{
    return strncmp(BOARD_HAL_DISPLAY_TYPE, "gc", 2) == 0;
}

// CIE L* (0..100) of a relative luminance Y (0..1)
static float lstar_from_y(float y)
{
    return y > 0.008856f ? 116.0f * cbrtf(y) - 16.0f : 903.3f * y;
}

// 8-bit sRGB neutral gray for a CIE L*
static uint8_t gray_from_lstar(float lstar)
{
    float y = lstar > 8.0f ? powf((lstar + 16.0f) / 116.0f, 3.0f) : lstar / 903.3f;
    float s = y <= 0.0031308f ? 12.92f * y : 1.055f * powf(y, 1.0f / 2.4f) - 0.055f;
    int v = (int) roundf(s * 255.0f);
    return (uint8_t) (v < 0 ? 0 : (v > 255 ? 255 : v));
}

static esp_err_t load_calibrated_palette(void)
{
    color_palette_t palette;
    esp_err_t err = color_palette_load(&palette);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load palette: %s", esp_err_to_name(err));
        return err;
    }

    // Update palette_measured array with loaded values (stored or defaults)
    palette_measured[0] = (rgb_t){palette.black.r, palette.black.g, palette.black.b};
    palette_measured[1] = (rgb_t){palette.white.r, palette.white.g, palette.white.b};
    palette_measured[2] = (rgb_t){palette.yellow.r, palette.yellow.g, palette.yellow.b};
    palette_measured[3] = (rgb_t){palette.red.r, palette.red.g, palette.red.b};
    palette_measured[5] = (rgb_t){palette.blue.r, palette.blue.g, palette.blue.b};
    palette_measured[6] = (rgb_t){palette.green.r, palette.green.g, palette.green.b};

    if (board_is_grayscale()) {
        // Build the measured 16-level ramp between the calibrated luminance
        // endpoints -- same math as buildCalibratedGrayRamp() in
        // epaper-image-convert, so device-side dithering matches the tools.
        float black_l = lstar_from_y(palette.gray_black_y);
        float white_l = lstar_from_y(palette.gray_white_y);
        float gamma = palette.gray_gamma > 0.0f ? palette.gray_gamma : 1.0f;
        for (int i = 0; i < 16; i++) {
            float t = (float) i / 15.0f;
            float shaped = powf(t, gamma);
            uint8_t v = gray_from_lstar(black_l + shaped * (white_l - black_l));
            gray_measured[i] = (rgb_t){v, v, v};
        }
    }

    return ESP_OK;
}

// Precomputed LUTs for sRGB <-> linear conversion
#define LINEAR_TO_SRGB_SIZE 4096
static float srgb_to_linear_lut[256];
static uint8_t linear_to_srgb_lut[LINEAR_TO_SRGB_SIZE];
static bool luts_initialized = false;

static void init_gamma_luts(void)
{
    if (luts_initialized) {
        return;
    }

    // sRGB byte -> linear float
    for (int i = 0; i < 256; i++) {
        float s = i / 255.0f;
        srgb_to_linear_lut[i] = s > 0.04045f ? powf((s + 0.055f) / 1.055f, 2.4f) : s / 12.92f;
    }

    // linear float (scaled to 0..4095) -> sRGB byte
    for (int i = 0; i < LINEAR_TO_SRGB_SIZE; i++) {
        float lin = (float) i / (LINEAR_TO_SRGB_SIZE - 1);
        float s = lin > 0.0031308f ? 1.055f * powf(lin, 1.0f / 2.4f) - 0.055f : 12.92f * lin;
        int v = (int) roundf(s * 255.0f);
        linear_to_srgb_lut[i] = (uint8_t) (v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    luts_initialized = true;
}

static inline float srgb_to_linear(uint8_t v)
{
    return srgb_to_linear_lut[v];
}

static inline uint8_t linear_to_srgb(float lin)
{
    if (lin <= 0.0f)
        return 0;
    if (lin >= 1.0f)
        return 255;
    int idx = (int) (lin * (LINEAR_TO_SRGB_SIZE - 1) + 0.5f);
    return linear_to_srgb_lut[idx];
}

// Compressed Dynamic Range: remap each channel in linear light onto the
// panel's measured black..white range (relative colorimetric intent, same
// algorithm as epaper-image-convert). Pure white lands exactly on the
// measured white point and dithers with zero error -- no colored speckle on
// white backgrounds -- and chromatic adaptation hides the panel white's
// slight tint from the viewer. The mapping is a fixed per-channel byte
// transform, so it collapses into three 256-entry LUTs built once per
// image.
typedef struct {
    uint8_t lut[3][256];
} cdr_state_t;

// Exact conversion for LUT building (the fast 4096-entry linear_to_srgb
// table would quantize the 256 entries and break byte parity with the tool)
static uint8_t linear_to_srgb_exact(float lin)
{
    if (lin <= 0.0f)
        return 0;
    if (lin >= 1.0f)
        return 255;
    float s = lin > 0.0031308f ? 1.055f * powf(lin, 1.0f / 2.4f) - 0.055f : 12.92f * lin;
    int v = (int) roundf(s * 255.0f);
    return (uint8_t) (v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void cdr_init(cdr_state_t *cdr)
{
    init_gamma_luts();

    const rgb_t *mb = board_is_grayscale() ? &gray_measured[0] : &palette_measured[0];
    const rgb_t *mw = board_is_grayscale() ? &gray_measured[15] : &palette_measured[1];
    const uint8_t black[3] = {mb->r, mb->g, mb->b};
    const uint8_t white[3] = {mw->r, mw->g, mw->b};

    for (int c = 0; c < 3; c++) {
        float black_lin = srgb_to_linear(black[c]);
        float range = srgb_to_linear(white[c]) - black_lin;
        for (int v = 0; v < 256; v++) {
            cdr->lut[c][v] = linear_to_srgb_exact(black_lin + srgb_to_linear((uint8_t) v) * range);
        }
    }

    ESP_LOGI(TAG, "CDR: per-channel remap to (%d,%d,%d)..(%d,%d,%d)", black[0], black[1], black[2],
             white[0], white[1], white[2]);
}

static void cdr_apply_row(const cdr_state_t *cdr, uint8_t *row, int width)
{
    for (int x = 0; x < width; x++) {
        int idx = x * 3;
        row[idx] = cdr->lut[0][row[idx]];
        row[idx + 1] = cdr->lut[1][row[idx + 1]];
        row[idx + 2] = cdr->lut[2][row[idx + 2]];
    }
}

static int find_closest_color(uint8_t r, uint8_t g, uint8_t b, const rgb_t *pal)
{
    int min_dist = INT_MAX;
    int closest = 1;

    for (int i = 0; i < 7; i++) {
        if (i == 4)
            continue;

        int dr = r - pal[i].r;
        int dg = g - pal[i].g;
        int db = b - pal[i].b;
        int dist = dr * dr + dg * dg + db * db;

        if (dist < min_dist) {
            min_dist = dist;
            closest = i;
        }
    }

    return closest;
}

static int find_closest_gray16(uint8_t r, uint8_t g, uint8_t b)
{
    int min_dist = INT_MAX;
    int closest = 15;

    for (int i = 0; i < 16; i++) {
        int dr = r - gray_measured[i].r;
        int dg = g - gray_measured[i].g;
        int db = b - gray_measured[i].b;
        int dist = dr * dr + dg * dg + db * db;

        if (dist < min_dist) {
            min_dist = dist;
            closest = i;
        }
    }

    return closest;
}

// Error diffusion matrices, {dx, dy, numerator, denominator}
static const error_diffusion_t floyd_steinberg_matrix[] = {
    {1, 0, 7, 16}, {-1, 1, 3, 16}, {0, 1, 5, 16}, {1, 1, 1, 16}};
static const error_diffusion_t stucki_matrix[] = {
    {1, 0, 8, 42}, {2, 0, 4, 42},  {-2, 1, 2, 42}, {-1, 1, 4, 42}, {0, 1, 8, 42}, {1, 1, 4, 42},
    {2, 1, 2, 42}, {-2, 2, 1, 42}, {-1, 2, 2, 42}, {0, 2, 4, 42},  {1, 2, 2, 42}, {2, 2, 1, 42}};
static const error_diffusion_t burkes_matrix[] = {{1, 0, 8, 32},  {2, 0, 4, 32}, {-2, 1, 2, 32},
                                                  {-1, 1, 4, 32}, {0, 1, 8, 32}, {1, 1, 4, 32},
                                                  {2, 1, 2, 32}};
static const error_diffusion_t sierra_matrix[] = {
    {1, 0, 5, 32}, {2, 0, 3, 32}, {-2, 1, 2, 32}, {-1, 1, 4, 32}, {0, 1, 5, 32},
    {1, 1, 4, 32}, {2, 1, 2, 32}, {-1, 2, 2, 32}, {0, 2, 3, 32},  {1, 2, 2, 32}};

// Row-streaming error-diffusion state. Three scanline error buffers support
// matrices that diffuse up to dy=2 (Stucki, Sierra); rows must be fed strictly
// top to bottom.
//
// On grayscale (GC16) panels the working value and diffused error live in
// LINEAR LIGHT while nearest-level matching still happens in sRGB -- the same
// hybrid as applyErrorDiffusionDither() in epaper-image-convert: the eye
// averages the linear luminance of the dithered dots, so with only 16 levels
// gamma-space error accounting renders visibly wrong tones. Spectra panels
// keep the converter's tuned sRGB-space behavior (linear RGB is a poor
// perceptual space for mixing six saturated inks).
typedef struct {
    int width;
    bool grayscale;
    float hi;  // clamp ceiling of the working domain: 1.0 linear, 255.0 sRGB
    const error_diffusion_t *matrix;
    int matrix_size;
    float match_work[16][3];  // measured palette in the working domain
    float *curr_errors;
    float *next_errors;
    float *next2_errors;
} dither_state_t;

static esp_err_t dither_init(dither_state_t *st, int width, dither_algorithm_t algorithm)
{
    init_gamma_luts();

    st->width = width;
    st->grayscale = board_is_grayscale();
    st->hi = st->grayscale ? 1.0f : 255.0f;

    if (st->grayscale) {
        for (int i = 0; i < 16; i++) {
            st->match_work[i][0] = srgb_to_linear(gray_measured[i].r);
            st->match_work[i][1] = srgb_to_linear(gray_measured[i].g);
            st->match_work[i][2] = srgb_to_linear(gray_measured[i].b);
        }
    } else {
        for (int i = 0; i < 7; i++) {
            st->match_work[i][0] = (float) palette_measured[i].r;
            st->match_work[i][1] = (float) palette_measured[i].g;
            st->match_work[i][2] = (float) palette_measured[i].b;
        }
    }

    switch (algorithm) {
    case DITHER_STUCKI:
        st->matrix = stucki_matrix;
        st->matrix_size = sizeof(stucki_matrix) / sizeof(error_diffusion_t);
        break;
    case DITHER_BURKES:
        st->matrix = burkes_matrix;
        st->matrix_size = sizeof(burkes_matrix) / sizeof(error_diffusion_t);
        break;
    case DITHER_SIERRA:
        st->matrix = sierra_matrix;
        st->matrix_size = sizeof(sierra_matrix) / sizeof(error_diffusion_t);
        break;
    case DITHER_FLOYD_STEINBERG:
    default:
        st->matrix = floyd_steinberg_matrix;
        st->matrix_size = sizeof(floyd_steinberg_matrix) / sizeof(error_diffusion_t);
        break;
    }

    st->curr_errors = (float *) heap_caps_calloc(width * 3, sizeof(float), MALLOC_CAP_SPIRAM);
    st->next_errors = (float *) heap_caps_calloc(width * 3, sizeof(float), MALLOC_CAP_SPIRAM);
    st->next2_errors = (float *) heap_caps_calloc(width * 3, sizeof(float), MALLOC_CAP_SPIRAM);

    if (!st->curr_errors || !st->next_errors || !st->next2_errors) {
        ESP_LOGE(TAG, "Failed to allocate error buffers");
        if (st->curr_errors)
            free(st->curr_errors);
        if (st->next_errors)
            free(st->next_errors);
        if (st->next2_errors)
            free(st->next2_errors);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void dither_free(dither_state_t *st)
{
    free(st->curr_errors);
    free(st->next_errors);
    free(st->next2_errors);
}

static void dither_row(dither_state_t *st, uint8_t *row)
{
    for (int x = 0; x < st->width; x++) {
        int idx = x * 3;

        // Working value = decoded pixel + accumulated error, in the working
        // domain (linear light on grayscale, sRGB on Spectra)
        float w[3];
        for (int c = 0; c < 3; c++) {
            float v = st->grayscale ? srgb_to_linear(row[idx + c]) : (float) row[idx + c];
            v += st->curr_errors[idx + c];
            w[c] = v < 0.0f ? 0.0f : (v > st->hi ? st->hi : v);
        }

        // Nearest-level matching happens in sRGB against the measured
        // palette; output the theoretical color (what the firmware decode
        // paths expect); diffuse the error relative to the measured palette
        // in the working domain.
        int level;
        const rgb_t *out;
        if (st->grayscale) {
            level = find_closest_gray16(linear_to_srgb(w[0]), linear_to_srgb(w[1]),
                                        linear_to_srgb(w[2]));
            out = &gray_theoretical[level];
        } else {
            level = find_closest_color((uint8_t) (w[0] + 0.5f), (uint8_t) (w[1] + 0.5f),
                                       (uint8_t) (w[2] + 0.5f), palette_measured);
            out = &palette[level];
        }

        row[idx] = out->r;
        row[idx + 1] = out->g;
        row[idx + 2] = out->b;

        float err[3];
        for (int c = 0; c < 3; c++) {
            err[c] = w[c] - st->match_work[level][c];
        }

        // Distribute error to neighboring pixels using selected algorithm
        for (int i = 0; i < st->matrix_size; i++) {
            int nx = x + st->matrix[i].dx;
            if (nx < 0 || nx >= st->width) {
                continue;
            }

            float *target_errors;
            if (st->matrix[i].dy == 0) {
                target_errors = st->curr_errors;
            } else if (st->matrix[i].dy == 1) {
                target_errors = st->next_errors;
            } else {
                target_errors = st->next2_errors;
            }

            float weight = (float) st->matrix[i].numerator / (float) st->matrix[i].denominator;
            int target_idx = nx * 3;
            target_errors[target_idx] += err[0] * weight;
            target_errors[target_idx + 1] += err[1] * weight;
            target_errors[target_idx + 2] += err[2] * weight;
        }
    }

    // Rotate error buffers for the next row
    float *temp = st->curr_errors;
    st->curr_errors = st->next_errors;
    st->next_errors = st->next2_errors;
    st->next2_errors = temp;
    memset(st->next2_errors, 0, st->width * 3 * sizeof(float));
}

esp_err_t image_processor_init(void)
{
    load_calibrated_palette();
    ESP_LOGI(TAG, "Image processor initialized");
    return ESP_OK;
}

esp_err_t image_processor_reload_palette(void)
{
    esp_err_t err = load_calibrated_palette();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reload calibrated palette");
        return err;
    }
    ESP_LOGI(TAG, "Calibrated palette reloaded");
    return ESP_OK;
}

// ---- Row-streaming processing pipeline ----
//
// Geometry (orientation rotate + cover-crop resize), CDR, error-diffusion
// dithering, and both consumers (PNG encode, display paint) are all
// row-local, so the panel-size output is never materialized: each output
// row goes straight to a sink. The previous pipeline allocated two full
// panel-size RGB888 intermediates (~15 MB peak on the 1872x1404 GC16
// panel), which cannot fit in 8 MB of PSRAM; peak memory here is the
// decoded source plus a few scanlines.

typedef esp_err_t (*row_sink_fn)(void *ctx, int y, const uint8_t *row);

// The user's configured display orientation decides rotation -- the source
// image is never auto-rotated based on its own aspect ratio. This matches
// epaper-image-convert, which processes at the configured orientation's
// dimensions and rotates the result back into the native panel layout.
static bool orientation_needs_rotation(void)
{
    bool native_is_landscape = BOARD_HAL_DISPLAY_WIDTH > BOARD_HAL_DISPLAY_HEIGHT;
    bool orient_is_landscape =
        config_manager_get_display_orientation() == DISPLAY_ORIENTATION_LANDSCAPE;
    return native_is_landscape != orient_is_landscape;
}

typedef struct {
    const uint8_t *src;
    int src_w;
    int src_h;
    bool rotate;  // configured orientation differs from the native layout
    int proc_w;   // processing-space dimensions (native, swapped when rotated)
    int proc_h;
    float scale;
    int off_x;
    int off_y;
    bool box;  // downscale: box-average the source footprint; upscale: bilinear
    // Fit (letterbox) mode: pixels outside the processing-space content rect
    // are background bars
    bool fit;
    int content_x0;
    int content_y0;
    int content_x1;
    int content_y1;
    uint8_t bg[3];      // theoretical background, fed through CDR + dither
    uint8_t bg_out[3];  // exact output-palette background, repainted post-dither
    // Emitted row geometry: native panel rows by default, processing-space
    // rows when the sink accepts processing order (see
    // geometry_set_processing_order)
    int out_w;
    int out_h;
    bool processing_order;
    // Optional row provider for streamed sources; when set, src is unused
    // and rows are fetched on demand with monotonically non-decreasing
    // minimum row index
    const uint8_t *(*get_row)(void *ctx, int src_y);
    void *row_ctx;
} geometry_t;

// Map the background name to its theoretical output color. Only white and
// black are supported; anything else falls back to white.
static void background_theoretical_rgb(const char *name, uint8_t rgb[3])
{
    uint8_t v = strcmp(name, "black") == 0 ? 0 : 255;
    rgb[0] = rgb[1] = rgb[2] = v;
}

// rotate is passed in (not re-read from config) so one snapshot governs the
// whole pass -- geometry, decoder gating, and sink must agree even if the
// user flips the orientation setting mid-stream
static void geometry_init(geometry_t *geo, const uint8_t *src, int src_w, int src_h, bool rotate)
{
    geo->src = src;
    geo->src_w = src_w;
    geo->src_h = src_h;
    geo->get_row = NULL;
    geo->row_ctx = NULL;

    geo->rotate = rotate;
    geo->proc_w = geo->rotate ? BOARD_HAL_DISPLAY_HEIGHT : BOARD_HAL_DISPLAY_WIDTH;
    geo->proc_h = geo->rotate ? BOARD_HAL_DISPLAY_WIDTH : BOARD_HAL_DISPLAY_HEIGHT;
    geo->out_w = BOARD_HAL_DISPLAY_WIDTH;
    geo->out_h = BOARD_HAL_DISPLAY_HEIGHT;
    geo->processing_order = false;

    // Cover mode: scale to fill the processing space, center-crop the excess
    float scale_x = (float) geo->proc_w / src_w;
    float scale_y = (float) geo->proc_h / src_h;
    geo->scale = fmaxf(scale_x, scale_y);
    geo->off_x = ((int) (src_w * geo->scale) - geo->proc_w) / 2;
    geo->off_y = ((int) (src_h * geo->scale) - geo->proc_h) / 2;

    // Fit mode: scale to fit inside the processing space instead, centering
    // the content and letterboxing the rest with the configured background
    // color (the same layout epaper-image-convert's scaleMode "fit"
    // produces). The resample math is shared with cover mode -- off_x/off_y
    // just become the (negative) content origin.
    char bg_name[12] = "";
    geo->fit = processing_settings_get_scale_mode() == SCALE_MODE_FIT;
    geo->content_x0 = 0;
    geo->content_y0 = 0;
    geo->content_x1 = geo->proc_w;
    geo->content_y1 = geo->proc_h;
    if (geo->fit) {
        geo->scale = fminf(scale_x, scale_y);
        int content_w = (int) (src_w * geo->scale + 0.5f);
        int content_h = (int) (src_h * geo->scale + 0.5f);
        if (content_w > geo->proc_w)
            content_w = geo->proc_w;
        if (content_h > geo->proc_h)
            content_h = geo->proc_h;
        geo->content_x0 = (geo->proc_w - content_w) / 2;
        geo->content_y0 = (geo->proc_h - content_h) / 2;
        geo->content_x1 = geo->content_x0 + content_w;
        geo->content_y1 = geo->content_y0 + content_h;
        geo->off_x = -geo->content_x0;
        geo->off_y = -geo->content_y0;

        processing_settings_get_background_color(bg_name, sizeof(bg_name));
        background_theoretical_rgb(bg_name, geo->bg);
        if (board_is_grayscale()) {
            // Grayscale output palette: quantize the background to its ramp
            // level the same way content pixels are matched
            int level = find_closest_gray16(geo->bg[0], geo->bg[1], geo->bg[2]);
            geo->bg_out[0] = gray_theoretical[level].r;
            geo->bg_out[1] = gray_theoretical[level].g;
            geo->bg_out[2] = gray_theoretical[level].b;
        } else {
            memcpy(geo->bg_out, geo->bg, sizeof(geo->bg_out));
        }
    }

    // Resampling policy, matching epaper-image-convert's canvas resize:
    // downscales box-average the footprint so the ditherer receives correct
    // local means (nearest-neighbor skips pixels -- thin features vanish and
    // pre-dithered sources alias into moire), upscales interpolate
    // bilinearly. Both operate in gamma space, like canvas drawImage, so
    // device-processed and tool-converted images resample identically.
    geo->box = geo->scale < 1.0f;

    if (geo->fit) {
        ESP_LOGI(
            TAG, "Geometry: %dx%d -> %dx%d%s, fit: content %dx%d at (%d,%d) on %s, scale %.2f, %s",
            src_w, src_h, geo->proc_w, geo->proc_h, geo->rotate ? " (rotated to native)" : "",
            geo->content_x1 - geo->content_x0, geo->content_y1 - geo->content_y0, geo->content_x0,
            geo->content_y0, bg_name, geo->scale, geo->box ? "box" : "bilinear");
    } else {
        ESP_LOGI(TAG, "Geometry: %dx%d -> %dx%d%s, cover, scale %.2f, offset (%d,%d), %s", src_w,
                 src_h, geo->proc_w, geo->proc_h, geo->rotate ? " (rotated to native)" : "",
                 geo->scale, geo->off_x, geo->off_y, geo->box ? "box" : "bilinear");
    }
}

// Emit rows in processing-space order instead of native order: the sink
// receives proc_h rows of proc_w pixels and places them itself (as native
// columns when rotated). Source-row access then stays monotonic -- which
// streamed decoding requires even for rotated output -- and error diffusion
// runs in the same scanline order as epaper-image-convert's rotated output.
static void geometry_set_processing_order(geometry_t *geo)
{
    geo->processing_order = true;
    geo->out_w = geo->proc_w;
    geo->out_h = geo->proc_h;
}

// Fetch a source pixel, clamped to the source bounds
static const uint8_t *geometry_src_pixel(const geometry_t *geo, int sx, int sy)
{
    if (sx < 0)
        sx = 0;
    if (sx >= geo->src_w)
        sx = geo->src_w - 1;
    if (sy < 0)
        sy = 0;
    if (sy >= geo->src_h)
        sy = geo->src_h - 1;

    const uint8_t *row =
        geo->get_row ? geo->get_row(geo->row_ctx, sy) : geo->src + (size_t) sy * geo->src_w * 3;
    return row + sx * 3;
}

static void geometry_fill_row(const geometry_t *geo, int out_y, uint8_t *row)
{
    for (int out_x = 0; out_x < geo->out_w; out_x++) {
        uint8_t *out = &row[out_x * 3];

        // Map the output pixel into processing space. In processing order
        // this is the identity; in native order the rotation follows the
        // same direction the old materialized pipeline used, so mounted
        // frames keep their orientation.
        int x, y;
        if (geo->rotate && !geo->processing_order) {
            x = out_y;
            y = geo->proc_h - 1 - out_x;
        } else {
            x = out_x;
            y = out_y;
        }

        if (geo->fit && (x < geo->content_x0 || x >= geo->content_x1 || y < geo->content_y0 ||
                         y >= geo->content_y1)) {
            out[0] = geo->bg[0];
            out[1] = geo->bg[1];
            out[2] = geo->bg[2];
            continue;
        }

        if (geo->box) {
            // Box average over the output pixel's source footprint, with
            // boundary pixels weighted by their overlap area so non-integer
            // scales resample like canvas instead of blurring
            float x0f = (x + geo->off_x) / geo->scale;
            float x1f = (x + 1 + geo->off_x) / geo->scale;
            float y0f = (y + geo->off_y) / geo->scale;
            float y1f = (y + 1 + geo->off_y) / geo->scale;
            int vx0 = (int) floorf(x0f);
            int vx1 = (int) ceilf(x1f);
            int vy0 = (int) floorf(y0f);
            int vy1 = (int) ceilf(y1f);

            float acc[3] = {0.0f, 0.0f, 0.0f};
            float wsum = 0.0f;
            for (int vy_i = vy0; vy_i < vy1; vy_i++) {
                float wy = fminf((float) (vy_i + 1), y1f) - fmaxf((float) vy_i, y0f);
                for (int vx_i = vx0; vx_i < vx1; vx_i++) {
                    float wx = fminf((float) (vx_i + 1), x1f) - fmaxf((float) vx_i, x0f);
                    float w = wx * wy;
                    const uint8_t *p = geometry_src_pixel(geo, vx_i, vy_i);
                    acc[0] += p[0] * w;
                    acc[1] += p[1] * w;
                    acc[2] += p[2] * w;
                    wsum += w;
                }
            }
            out[0] = (uint8_t) (acc[0] / wsum + 0.5f);
            out[1] = (uint8_t) (acc[1] / wsum + 0.5f);
            out[2] = (uint8_t) (acc[2] / wsum + 0.5f);
        } else {
            // Center-aligned bilinear between the four nearest source pixels
            float fx = (x + 0.5f + geo->off_x) / geo->scale - 0.5f;
            float fy = (y + 0.5f + geo->off_y) / geo->scale - 0.5f;
            int vx0 = (int) floorf(fx);
            int vy0 = (int) floorf(fy);
            float tx = fx - vx0;
            float ty = fy - vy0;

            const uint8_t *p00 = geometry_src_pixel(geo, vx0, vy0);
            const uint8_t *p10 = geometry_src_pixel(geo, vx0 + 1, vy0);
            const uint8_t *p01 = geometry_src_pixel(geo, vx0, vy0 + 1);
            const uint8_t *p11 = geometry_src_pixel(geo, vx0 + 1, vy0 + 1);
            for (int c = 0; c < 3; c++) {
                float top = p00[c] * (1.0f - tx) + p10[c] * tx;
                float bot = p01[c] * (1.0f - tx) + p11[c] * tx;
                out[c] = (uint8_t) (top * (1.0f - ty) + bot * ty + 0.5f);
            }
        }
    }
}

// Restore fit-mode letterbox bars to the exact output palette color after
// dithering (the row-local equivalent of epaper-image-convert's background
// repaint): diffusion error crossing the content boundary must not leave
// stray dots in the bars.
static void geometry_repaint_background(const geometry_t *geo, int out_y, uint8_t *row)
{
    if (!geo->fit) {
        return;
    }
    for (int out_x = 0; out_x < geo->out_w; out_x++) {
        int x, y;
        if (geo->rotate && !geo->processing_order) {
            x = out_y;
            y = geo->proc_h - 1 - out_x;
        } else {
            x = out_x;
            y = out_y;
        }
        if (x < geo->content_x0 || x >= geo->content_x1 || y < geo->content_y0 ||
            y >= geo->content_y1) {
            row[out_x * 3] = geo->bg_out[0];
            row[out_x * 3 + 1] = geo->bg_out[1];
            row[out_x * 3 + 2] = geo->bg_out[2];
        }
    }
}

static esp_err_t run_stream(geometry_t *geo, dither_algorithm_t dither_algorithm, row_sink_fn sink,
                            void *sink_ctx)
{
    // The LUTs are too large for the task stack
    cdr_state_t *cdr = (cdr_state_t *) heap_caps_malloc(sizeof(cdr_state_t), MALLOC_CAP_SPIRAM);
    if (!cdr) {
        ESP_LOGE(TAG, "Failed to allocate CDR state");
        return ESP_ERR_NO_MEM;
    }
    cdr_init(cdr);

    dither_state_t dither;
    esp_err_t err = dither_init(&dither, geo->out_w, dither_algorithm);
    if (err != ESP_OK) {
        heap_caps_free(cdr);
        return err;
    }

    uint8_t *row = (uint8_t *) heap_caps_malloc(geo->out_w * 3, MALLOC_CAP_SPIRAM);
    if (!row) {
        ESP_LOGE(TAG, "Failed to allocate row buffer");
        dither_free(&dither);
        heap_caps_free(cdr);
        return ESP_ERR_NO_MEM;
    }

    for (int y = 0; y < geo->out_h && err == ESP_OK; y++) {
        geometry_fill_row(geo, y, row);
        cdr_apply_row(cdr, row, geo->out_w);
        dither_row(&dither, row);
        geometry_repaint_background(geo, y, row);
        err = sink(sink_ctx, y, row);

        // Yield periodically so the IDLE task can feed the watchdog; dense
        // enough that the sleep windows overlap with other busy tasks'
        // yields (idle only runs when every higher-priority task sleeps
        // in the same tick)
        if ((y & 7) == 0) {
            vTaskDelay(1);
        }
    }

    heap_caps_free(row);
    dither_free(&dither);
    heap_caps_free(cdr);
    return err;
}

static esp_err_t process_rgb_stream(const uint8_t *rgb_buffer, int width, int height,
                                    dither_algorithm_t dither_algorithm, row_sink_fn sink,
                                    void *sink_ctx, bool processing_order, bool rotated)
{
    ESP_LOGI(TAG, "Processing RGB buffer: %dx%d", width, height);

    geometry_t geo;
    geometry_init(&geo, rgb_buffer, width, height, rotated);
    if (processing_order) {
        geometry_set_processing_order(&geo);
    }
    return run_stream(&geo, dither_algorithm, sink, sink_ctx);
}

// Streaming PNG writer -- rows are written to the file as they are produced
typedef struct {
    FILE *fp;
    png_structp png_ptr;
    png_infop info_ptr;
} png_writer_t;

static esp_err_t png_writer_open(png_writer_t *pw, const char *filename, int width, int height)
{
    memset(pw, 0, sizeof(*pw));

    pw->fp = fopen(filename, "wb");
    if (!pw->fp) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filename);
        return ESP_FAIL;
    }

    pw->png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!pw->png_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG write struct");
        fclose(pw->fp);
        pw->fp = NULL;
        return ESP_FAIL;
    }

    pw->info_ptr = png_create_info_struct(pw->png_ptr);
    if (!pw->info_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG info struct");
        png_destroy_write_struct(&pw->png_ptr, NULL);
        fclose(pw->fp);
        memset(pw, 0, sizeof(*pw));
        return ESP_FAIL;
    }

    if (setjmp(png_jmpbuf(pw->png_ptr))) {
        ESP_LOGE(TAG, "PNG encoding error");
        png_destroy_write_struct(&pw->png_ptr, &pw->info_ptr);
        fclose(pw->fp);
        memset(pw, 0, sizeof(*pw));
        return ESP_FAIL;
    }

    png_init_io(pw->png_ptr, pw->fp);
    png_set_IHDR(pw->png_ptr, pw->info_ptr, width, height, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(pw->png_ptr, pw->info_ptr);

    return ESP_OK;
}

static esp_err_t png_writer_row_sink(void *ctx, int y, const uint8_t *row)
{
    (void) y;
    png_writer_t *pw = (png_writer_t *) ctx;

    if (setjmp(png_jmpbuf(pw->png_ptr))) {
        ESP_LOGE(TAG, "PNG encoding error");
        return ESP_FAIL;
    }

    png_write_row(pw->png_ptr, (png_bytep) row);
    return ESP_OK;
}

static esp_err_t png_writer_close(png_writer_t *pw, bool success)
{
    // volatile: modified between setjmp and a potential longjmp from libpng
    volatile esp_err_t err = success ? ESP_OK : ESP_FAIL;

    if (pw->png_ptr) {
        if (success) {
            if (setjmp(png_jmpbuf(pw->png_ptr))) {
                ESP_LOGE(TAG, "PNG encoding error");
                err = ESP_FAIL;
            } else {
                png_write_end(pw->png_ptr, NULL);
            }
        }
        png_destroy_write_struct(&pw->png_ptr, &pw->info_ptr);
    }
    if (pw->fp) {
        fclose(pw->fp);
    }
    memset(pw, 0, sizeof(*pw));

    return err;
}

// Decode JPG from buffer to RGB
static esp_err_t decode_jpg_buffer(const uint8_t *jpg_data, size_t jpg_size, uint8_t **rgb_buffer,
                                   int *width, int *height)
{
    esp_jpeg_image_cfg_t jpeg_cfg = {.indata = (uint8_t *) jpg_data,
                                     .indata_size = jpg_size,
                                     .out_format = JPEG_IMAGE_FORMAT_RGB888,
                                     .out_scale = JPEG_IMAGE_SCALE_0};
    esp_jpeg_image_output_t outimg;
    esp_jpeg_get_image_info(&jpeg_cfg, &outimg);
    int original_width = outimg.width;
    int original_height = outimg.height;

    // Scaling logic - scale down large images to save memory
    if (outimg.width > BOARD_HAL_DISPLAY_WIDTH * 4 || outimg.height > BOARD_HAL_DISPLAY_HEIGHT * 4)
        jpeg_cfg.out_scale = JPEG_IMAGE_SCALE_1_4;
    else if (outimg.width > BOARD_HAL_DISPLAY_WIDTH * 2 ||
             outimg.height > BOARD_HAL_DISPLAY_HEIGHT * 2)
        jpeg_cfg.out_scale = JPEG_IMAGE_SCALE_1_2;

    if (jpeg_cfg.out_scale != JPEG_IMAGE_SCALE_0) {
        esp_jpeg_get_image_info(&jpeg_cfg, &outimg);
        ESP_LOGI(TAG, "JPG scaled from %dx%d to %dx%d (scale: 1/%d)", original_width,
                 original_height, outimg.width, outimg.height, 1 << jpeg_cfg.out_scale);
    } else {
        ESP_LOGI(TAG, "JPG size: %dx%d (no scaling needed)", outimg.width, outimg.height);
    }

    *rgb_buffer = (uint8_t *) heap_caps_malloc(outimg.output_len, MALLOC_CAP_SPIRAM);
    if (!*rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate JPG RGB buffer of %u bytes", outimg.output_len);
        return ESP_ERR_NO_MEM;
    }

    jpeg_cfg.outbuf = *rgb_buffer;
    jpeg_cfg.outbuf_size = outimg.output_len;
    esp_err_t decode_err = esp_jpeg_decode(&jpeg_cfg, &outimg);
    if (decode_err != ESP_OK) {
        ESP_LOGE(TAG, "JPG decoding failed: %s", esp_err_to_name(decode_err));
        set_last_error("JPG decoding failed");
        heap_caps_free(*rgb_buffer);
        *rgb_buffer = NULL;
        return ESP_FAIL;
    }

    *width = outimg.width;
    *height = outimg.height;
    return ESP_OK;
}

// PNG memory read callback structure
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} png_mem_read_t;

static void png_mem_read_callback(png_structp png_ptr, png_bytep data, png_size_t length)
{
    png_mem_read_t *mem = (png_mem_read_t *) png_get_io_ptr(png_ptr);
    if (mem->offset + length > mem->size) {
        png_error(png_ptr, "Read past end of buffer");
        return;
    }
    memcpy(data, mem->data + mem->offset, length);
    mem->offset += length;
}

// Read just the PNG header dimensions, so oversized images can be rejected
// before libpng allocates the full decoded image internally
static bool png_peek_dims(const uint8_t *png_data, size_t png_size, int *width, int *height)
{
    png_mem_read_t mem = {.data = png_data, .size = png_size, .offset = 0};

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        return false;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        return false;
    }
    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return false;
    }

    png_set_read_fn(png_ptr, &mem, png_mem_read_callback);
    png_read_info(png_ptr, info_ptr);
    *width = png_get_image_width(png_ptr, info_ptr);
    *height = png_get_image_height(png_ptr, info_ptr);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    return true;
}

// Decode PNG from buffer to RGB
static esp_err_t decode_png_buffer(const uint8_t *png_data, size_t png_size, uint8_t **rgb_buffer,
                                   int *width, int *height)
{
    // Gate on the decoded size BEFORE the full decode: png_read_png allocates
    // the whole image internally, so an oversized source would OOM inside
    // libpng with a generic failure instead of this specific one
    int peek_w = 0, peek_h = 0;
    if (png_peek_dims(png_data, png_size, &peek_w, &peek_h) &&
        (size_t) peek_w * peek_h * 3 > 6 * 1024 * 1024) {
        ESP_LOGE(TAG, "PNG image too large for memory: %dx%d (limit 6MB decoded)", peek_w, peek_h);
        return ESP_ERR_NO_MEM;
    }

    png_mem_read_t mem = {.data = png_data, .size = png_size, .offset = 0};

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG read struct");
        return ESP_FAIL;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG info struct");
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        return ESP_FAIL;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        ESP_LOGE(TAG, "PNG decoding error");
        set_last_error("PNG decoding error");
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return ESP_FAIL;
    }

    png_set_read_fn(png_ptr, &mem, png_mem_read_callback);
    png_read_png(png_ptr, info_ptr,
                 PNG_TRANSFORM_STRIP_16 | PNG_TRANSFORM_PACKING | PNG_TRANSFORM_EXPAND |
                     PNG_TRANSFORM_STRIP_ALPHA,
                 NULL);

    *width = png_get_image_width(png_ptr, info_ptr);
    *height = png_get_image_height(png_ptr, info_ptr);
    ESP_LOGI(TAG, "PNG Image info: %dx%d", *width, *height);

    size_t rgb_size = (*width) * (*height) * 3;
    if (rgb_size > 6 * 1024 * 1024) {
        ESP_LOGE(TAG, "PNG image too large for memory: %zu bytes (limit 6MB)", rgb_size);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return ESP_ERR_NO_MEM;
    }

    *rgb_buffer = (uint8_t *) heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM);
    if (!*rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate PNG RGB buffer of %zu bytes", rgb_size);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return ESP_ERR_NO_MEM;
    }

    png_bytep *row_pointers = png_get_rows(png_ptr, info_ptr);
    int channels = png_get_channels(png_ptr, info_ptr);
    if (channels != 3) {
        ESP_LOGE(TAG, "Unsupported channel count: %d", channels);
        set_last_error("Unsupported PNG pixel format");
        heap_caps_free(*rgb_buffer);
        *rgb_buffer = NULL;
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return ESP_FAIL;
    }

    for (int y = 0; y < *height; y++) {
        memcpy(*rgb_buffer + y * (*width) * 3, row_pointers[y], (*width) * 3);
    }

    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    return ESP_OK;
}

// ---- Streamed PNG source ----
//
// When the source needs no rotation, resampling only reads a short,
// monotonically advancing window of source rows, so the pipeline can pull
// rows from libpng on demand into a small ring buffer instead of decoding
// the whole image. That removes the width*height*3 source allocation (7.9 MB
// for a panel-size upload on the 1872x1404 GC16 board -- more than the free
// PSRAM). Rotated sources still use the buffered decoder: their first output
// row reads the last source column. Interlaced PNGs also fall back (passes
// cannot be composed row by row).

typedef struct {
    png_structp png_ptr;
    png_infop info_ptr;
    png_mem_read_t mem;
    uint8_t *ring;  // ring_rows decoded source rows
    int ring_rows;
    int width;
    int height;
    int rows_decoded;
    bool error;  // a row failed to decode; the pass must be failed
} png_stream_src_t;

static void png_stream_close(png_stream_src_t *src)
{
    if (src->png_ptr) {
        png_destroy_read_struct(&src->png_ptr, src->info_ptr ? &src->info_ptr : NULL, NULL);
    }
    if (src->ring) {
        heap_caps_free(src->ring);
    }
    memset(src, 0, sizeof(*src));
}

// Prepares a streamed read of png_data. On ESP_OK with *supported true, the
// caller must run png_stream_run() and then png_stream_close(); with
// *supported false the source needs the buffered path and src is already
// closed. png_data must stay valid until png_stream_close().
// native_row_order: the sink needs native panel rows (PNG file output), so a
// rotated configuration cannot stream; display sinks accept processing order
// and stream regardless of rotation.
static esp_err_t png_stream_open(png_stream_src_t *src, const uint8_t *png_data, size_t png_size,
                                 bool native_row_order, bool rotated, bool *supported)
{
    memset(src, 0, sizeof(*src));
    *supported = false;

    src->mem = (png_mem_read_t){.data = png_data, .size = png_size, .offset = 0};

    src->png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!src->png_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG read struct");
        return ESP_FAIL;
    }
    src->info_ptr = png_create_info_struct(src->png_ptr);
    if (!src->info_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG info struct");
        png_stream_close(src);
        return ESP_FAIL;
    }

    if (setjmp(png_jmpbuf(src->png_ptr))) {
        ESP_LOGE(TAG, "PNG decoding error");
        set_last_error("PNG decoding error");
        png_stream_close(src);
        return ESP_FAIL;
    }

    png_set_read_fn(src->png_ptr, &src->mem, png_mem_read_callback);
    png_read_info(src->png_ptr, src->info_ptr);

    src->width = png_get_image_width(src->png_ptr, src->info_ptr);
    src->height = png_get_image_height(src->png_ptr, src->info_ptr);

    // The streamed path needs no decoded-size buffer, but work still scales
    // with source pixels; cap dimensions so a tiny, highly compressible
    // upload can't demand minutes of resampling (and a multi-megabyte ring)
    if (src->width <= 0 || src->height <= 0 || src->width > 16384 || src->height > 16384 ||
        (int64_t) src->width * src->height > 24 * 1024 * 1024) {
        ESP_LOGE(TAG, "PNG too large to process: %dx%d", src->width, src->height);
        set_last_error("Image dimensions too large");
        png_stream_close(src);
        return ESP_ERR_INVALID_SIZE;
    }

    if ((native_row_order && rotated) ||
        png_get_interlace_type(src->png_ptr, src->info_ptr) != PNG_INTERLACE_NONE) {
        png_stream_close(src);
        return ESP_OK;
    }

    // Normalize to RGB888, mirroring decode_png_buffer's transforms
    png_byte color_type = png_get_color_type(src->png_ptr, src->info_ptr);
    png_byte bit_depth = png_get_bit_depth(src->png_ptr, src->info_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(src->png_ptr);
    if (bit_depth < 8)
        png_set_packing(src->png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(src->png_ptr);
    bool has_trns = png_get_valid(src->png_ptr, src->info_ptr, PNG_INFO_tRNS) != 0;
    if (has_trns)
        png_set_tRNS_to_alpha(src->png_ptr);
    if (bit_depth == 16)
        png_set_strip_16(src->png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(src->png_ptr);
    // Strip alpha whether native or introduced by the tRNS expansion above
    if (color_type == PNG_COLOR_TYPE_RGB_ALPHA || color_type == PNG_COLOR_TYPE_GRAY_ALPHA ||
        has_trns)
        png_set_strip_alpha(src->png_ptr);
    png_read_update_info(src->png_ptr, src->info_ptr);

    if (png_get_channels(src->png_ptr, src->info_ptr) != 3) {
        // Let the buffered path report the unsupported layout
        png_stream_close(src);
        return ESP_OK;
    }

    // Ring sized for the widest resample window (box footprint), plus slack.
    // Fit mode scales by the SMALLER ratio, so each output row's footprint
    // spans more source rows than in cover mode; size for that
    // unconditionally rather than re-reading the scale-mode setting here (a
    // config flip mid-stream must not shrink the ring under the geometry).
    int proc_w = rotated ? BOARD_HAL_DISPLAY_HEIGHT : BOARD_HAL_DISPLAY_WIDTH;
    int proc_h = rotated ? BOARD_HAL_DISPLAY_WIDTH : BOARD_HAL_DISPLAY_HEIGHT;
    float scale_x = (float) proc_w / src->width;
    float scale_y = (float) proc_h / src->height;
    float scale = fminf(scale_x, scale_y);
    int window = scale < 1.0f ? (int) ceilf(1.0f / scale) + 2 : 3;
    if (window > src->height) {
        window = src->height;
    }
    src->ring = (uint8_t *) heap_caps_malloc((size_t) window * src->width * 3,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!src->ring) {
        ESP_LOGE(TAG, "Failed to allocate PNG stream ring buffer");
        png_stream_close(src);
        return ESP_ERR_NO_MEM;
    }
    src->ring_rows = window;

    *supported = true;
    return ESP_OK;
}

static const uint8_t *png_stream_get_row(void *ctx, int src_y)
{
    png_stream_src_t *src = (png_stream_src_t *) ctx;

    if (!src->error && src->rows_decoded <= src_y) {
        // Arm the longjmp target around the decode only, so a corrupt row
        // cannot unwind past run_stream's cleanup (leaking its row and error
        // buffers); on error the ring's stale content is served and the
        // whole pass is failed afterwards.
        if (setjmp(png_jmpbuf(src->png_ptr))) {
            ESP_LOGE(TAG, "PNG decoding error");
            set_last_error("PNG decoding error");
            src->error = true;
        } else {
            while (src->rows_decoded <= src_y) {
                uint8_t *slot =
                    src->ring + (size_t) (src->rows_decoded % src->ring_rows) * src->width * 3;
                png_read_row(src->png_ptr, (png_bytep) slot, NULL);
                src->rows_decoded++;
                // Cover-cropping an elongated source can skip far ahead in
                // one request; yield so the IDLE task can feed the watchdog
                if ((src->rows_decoded & 63) == 0) {
                    vTaskDelay(1);
                }
            }
        }
    }

    return src->ring + (size_t) (src_y % src->ring_rows) * src->width * 3;
}

static esp_err_t png_stream_run(png_stream_src_t *src, dither_algorithm_t dither_algorithm,
                                row_sink_fn sink, void *sink_ctx, bool processing_order,
                                bool rotated)
{
    ESP_LOGI(TAG, "Streaming PNG: %dx%d (%d-row window)", src->width, src->height, src->ring_rows);

    geometry_t geo;
    geometry_init(&geo, NULL, src->width, src->height, rotated);
    if (processing_order) {
        geometry_set_processing_order(&geo);
    }
    geo.get_row = png_stream_get_row;
    geo.row_ctx = src;

    esp_err_t err = run_stream(&geo, dither_algorithm, sink, sink_ctx);
    if (err == ESP_OK && src->error) {
        err = ESP_FAIL;
    }
    return err;
}

image_format_t image_processor_detect_format_buffer(const uint8_t *data, size_t size)
{
    if (size < 8) {
        return IMAGE_FORMAT_UNKNOWN;
    }

    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
        data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A) {
        return IMAGE_FORMAT_PNG;
    } else if (data[0] == 0x42 && data[1] == 0x4D) {
        return IMAGE_FORMAT_BMP;
    } else if (data[0] == 0xFF && data[1] == 0xD8) {
        return IMAGE_FORMAT_JPG;
    } else if (data[0] == 0x1F && data[1] == 0x8B) {
        return IMAGE_FORMAT_EPD_GZ;
    }

    return IMAGE_FORMAT_UNKNOWN;
}

// Display sinks always receive processing-order rows. Unrotated, those are
// native rows; rotated, each processing row is one native column (the paint
// buffer is random access, unlike a PNG being encoded).
typedef struct {
    bool rotated;
    int proc_w;
    int proc_h;
} display_sink_ctx_t;

static esp_err_t display_row_sink(void *ctx, int y, const uint8_t *row)
{
    display_sink_ctx_t *d = (display_sink_ctx_t *) ctx;
    if (d->rotated) {
        return display_manager_push_rgb_column(d->proc_h - 1 - y, row, d->proc_w);
    }
    return display_manager_push_rgb_row(y, row, BOARD_HAL_DISPLAY_WIDTH);
}

esp_err_t image_processor_process_to_display(const uint8_t *input_data, size_t input_size,
                                             image_format_t format,
                                             dither_algorithm_t dither_algorithm,
                                             const display_publish_t *pub)
{
    if (!input_data || input_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *algo_names[] = {"floyd-steinberg", "stucki", "burkes", "sierra"};
    ESP_LOGI(TAG, "Processing buffer to display (%zu bytes, format: %d, dither: %s)", input_size,
             format, algo_names[dither_algorithm]);

    last_error_msg[0] = '\0';

    esp_err_t err;

    // The display sink accepts processing-order rows, so PNGs stream
    // straight from the decoder regardless of rotation -- no source buffer,
    // no decoded-size limit
    display_sink_ctx_t sink_ctx = {
        .rotated = orientation_needs_rotation(),
        .proc_w = 0,
        .proc_h = 0,
    };
    sink_ctx.proc_w = sink_ctx.rotated ? BOARD_HAL_DISPLAY_HEIGHT : BOARD_HAL_DISPLAY_WIDTH;
    sink_ctx.proc_h = sink_ctx.rotated ? BOARD_HAL_DISPLAY_WIDTH : BOARD_HAL_DISPLAY_HEIGHT;

    if (format == IMAGE_FORMAT_PNG) {
        png_stream_src_t stream;
        bool streamable = false;
        err =
            png_stream_open(&stream, input_data, input_size, false, sink_ctx.rotated, &streamable);
        if (err != ESP_OK) {
            return err;
        }
        if (streamable) {
            err = display_manager_begin_rgb_stream();
            if (err == ESP_OK) {
                err = png_stream_run(&stream, dither_algorithm, display_row_sink, &sink_ctx, true,
                                     sink_ctx.rotated);
                {
                    esp_err_t end_err = display_manager_end_rgb_stream(err == ESP_OK, pub);
                    if (err == ESP_OK) {
                        err = end_err;
                    }
                }
            }
            png_stream_close(&stream);
            return err;
        }
        // Interlaced source: fall through to the buffered decode
    }

    // Decode input to RGB
    uint8_t *rgb_buffer = NULL;
    int width = 0, height = 0;

    if (format == IMAGE_FORMAT_JPG) {
        err = decode_jpg_buffer(input_data, input_size, &rgb_buffer, &width, &height);
    } else if (format == IMAGE_FORMAT_PNG) {
        err = decode_png_buffer(input_data, input_size, &rgb_buffer, &width, &height);
    } else {
        ESP_LOGE(TAG, "Unsupported image format for buffer processing: %d", format);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Decoded image: %dx%d", width, height);

    // Stream processed rows straight into the display buffer, then refresh
    err = display_manager_begin_rgb_stream();
    if (err == ESP_OK) {
        err = process_rgb_stream(rgb_buffer, width, height, dither_algorithm, display_row_sink,
                                 &sink_ctx, true, sink_ctx.rotated);

        // Every row has been painted; release the decoded source before end
        // runs the snapshot (its zlib state needs PSRAM a near-full decode
        // could otherwise deny)
        heap_caps_free(rgb_buffer);
        rgb_buffer = NULL;

        esp_err_t end_err = display_manager_end_rgb_stream(err == ESP_OK, pub);
        if (err == ESP_OK) {
            err = end_err;
        }
    }

    heap_caps_free(rgb_buffer);
    return err;
}

esp_err_t image_processor_process(const char *input_path, const char *output_path,
                                  dither_algorithm_t dither_algorithm)
{
    const char *algo_names[] = {"floyd-steinberg", "stucki", "burkes", "sierra"};
    ESP_LOGI(TAG, "Processing %s -> %s (dither: %s)", input_path, output_path,
             algo_names[dither_algorithm]);

    last_error_msg[0] = '\0';

    // Detect format first
    image_format_t format = image_processor_detect_format(input_path);
    if (format == IMAGE_FORMAT_UNKNOWN || format == IMAGE_FORMAT_BMP) {
        ESP_LOGE(TAG, "Unsupported image format for processing");
        return ESP_FAIL;
    }

    // Read entire file into buffer
    FILE *fp = fopen(input_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open input file: %s", input_path);
        return ESP_FAIL;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *file_buffer = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!file_buffer) {
        ESP_LOGE(TAG, "Failed to allocate file buffer of %ld bytes", file_size);
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    size_t read_bytes = fread(file_buffer, 1, file_size, fp);
    fclose(fp);

    if (read_bytes != file_size) {
        ESP_LOGE(TAG, "Failed to read entire file");
        heap_caps_free(file_buffer);
        return ESP_FAIL;
    }

    esp_err_t err;
    bool rotated = orientation_needs_rotation();

    // Non-rotated PNGs stream straight from the decoder into the output PNG
    if (format == IMAGE_FORMAT_PNG) {
        png_stream_src_t stream;
        bool streamable = false;
        err = png_stream_open(&stream, file_buffer, file_size, true, rotated, &streamable);
        if (err != ESP_OK) {
            heap_caps_free(file_buffer);
            return err;
        }
        if (streamable) {
            ESP_LOGI(TAG, "Writing PNG output to %s", output_path);
            png_writer_t writer;
            err = png_writer_open(&writer, output_path, BOARD_HAL_DISPLAY_WIDTH,
                                  BOARD_HAL_DISPLAY_HEIGHT);
            if (err == ESP_OK) {
                err = png_stream_run(&stream, dither_algorithm, png_writer_row_sink, &writer, false,
                                     rotated);
                esp_err_t close_err = png_writer_close(&writer, err == ESP_OK);
                if (err == ESP_OK) {
                    err = close_err;
                }
            }
            png_stream_close(&stream);
            heap_caps_free(file_buffer);

            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Successfully wrote PNG to %s", output_path);
            } else {
                unlink(output_path);
            }
            return err;
        }
        // Rotated or interlaced source: fall through to the buffered decode
    }

    // Decode to RGB buffer
    uint8_t *rgb_buffer = NULL;
    int width = 0, height = 0;

    if (format == IMAGE_FORMAT_JPG) {
        err = decode_jpg_buffer(file_buffer, file_size, &rgb_buffer, &width, &height);
    } else if (format == IMAGE_FORMAT_PNG) {
        err = decode_png_buffer(file_buffer, file_size, &rgb_buffer, &width, &height);
    } else {
        heap_caps_free(file_buffer);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Free input file buffer immediately after decoding
    heap_caps_free(file_buffer);

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Decoded image: %dx%d", width, height);

    // Stream processed rows straight into the output PNG
    ESP_LOGI(TAG, "Writing PNG output to %s", output_path);
    png_writer_t writer;
    err = png_writer_open(&writer, output_path, BOARD_HAL_DISPLAY_WIDTH, BOARD_HAL_DISPLAY_HEIGHT);
    if (err == ESP_OK) {
        err = process_rgb_stream(rgb_buffer, width, height, dither_algorithm, png_writer_row_sink,
                                 &writer, false, rotated);
        // Keep the processing error (e.g. ESP_ERR_NO_MEM, which callers map
        // to a specific response); only a failed finalize of an otherwise
        // successful write becomes the result.
        esp_err_t close_err = png_writer_close(&writer, err == ESP_OK);
        if (err == ESP_OK) {
            err = close_err;
        }
    }

    heap_caps_free(rgb_buffer);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Successfully wrote PNG to %s", output_path);
    } else {
        unlink(output_path);
    }

    return err;
}

// True when (r,g,b) is one of the theoretical output colors the processing
// pipeline emits for this board
static bool pixel_in_output_palette(uint8_t r, uint8_t g, uint8_t b)
{
    if (board_is_grayscale()) {
        // GC16: every ramp level is a neutral multiple of 17 (0, 17, ... 255)
        return r == g && g == b && r % 17 == 0;
    }
    for (int i = 0; i < 7; i++) {
        if (i == 4)
            continue;  // Skip reserved
        if (r == palette[i].r && g == palette[i].g && b == palette[i].b) {
            return true;
        }
    }
    return false;
}

// Shared row-streaming processed-image validation: verifies panel
// dimensions and that every pixel is a theoretical output color, one row at
// a time (a full-size GC16 frame decoded whole would need 7.9 MB). Expects a
// freshly created read struct with its IO source already attached; arms its
// own longjmp target, so callers only destroy the structs afterwards.
// With paint_rows set, each validated row is also pushed to the display
// stream, fusing the validation and display decodes into one pass; on a
// failed validation the partially painted buffer is simply not refreshed.
static bool check_processed_png(png_structp png_ptr, png_infop info_ptr, bool paint_rows)
{
    png_bytep volatile row = NULL;

    if (setjmp(png_jmpbuf(png_ptr))) {
        ESP_LOGE(TAG, "PNG error during check");
        free((void *) row);
        return false;
    }

    png_read_info(png_ptr, info_ptr);

    int width = png_get_image_width(png_ptr, info_ptr);
    int height = png_get_image_height(png_ptr, info_ptr);

    if (width != BOARD_HAL_DISPLAY_WIDTH || height != BOARD_HAL_DISPLAY_HEIGHT) {
        ESP_LOGI(TAG, "Dimensions mismatch: %dx%d (expected %dx%d)", width, height,
                 BOARD_HAL_DISPLAY_WIDTH, BOARD_HAL_DISPLAY_HEIGHT);
        return false;
    }

    // The processing pipeline always writes non-interlaced PNGs, so an Adam7
    // file cannot be our own output -- and single-pass row reads cannot
    // compose interlace passes anyway. Route it to processing instead.
    if (png_get_interlace_type(png_ptr, info_ptr) != PNG_INTERLACE_NONE) {
        ESP_LOGI(TAG, "Interlaced PNG, treating as unprocessed");
        return false;
    }

    // Force 8-bit RGB format (16-bit samples would double the row stride
    // under the 3-bytes-per-pixel walk below)
    png_set_expand(png_ptr);
    png_set_strip_16(png_ptr);
    png_set_strip_alpha(png_ptr);
    png_set_packing(png_ptr);
    png_set_palette_to_rgb(png_ptr);
    png_read_update_info(png_ptr, info_ptr);

    if (png_get_channels(png_ptr, info_ptr) != 3) {
        ESP_LOGI(TAG, "Not RGB format");
        return false;
    }

    // Check pixels row by row
    row = (png_bytep) malloc(png_get_rowbytes(png_ptr, info_ptr));
    if (!row) {
        ESP_LOGE(TAG, "Failed to allocate row buffer");
        return false;
    }

    bool valid = true;
    for (int y = 0; y < height && valid; y++) {
        png_read_row(png_ptr, row, NULL);

        for (int x = 0; x < width; x++) {
            if (!pixel_in_output_palette(row[x * 3], row[x * 3 + 1], row[x * 3 + 2])) {
                ESP_LOGI(TAG, "Pixel (%d,%d) color (%d,%d,%d) not in palette", x, y, row[x * 3],
                         row[x * 3 + 1], row[x * 3 + 2]);
                valid = false;
                break;
            }
        }

        if (valid && paint_rows) {
            display_manager_push_rgb_row(y, (const uint8_t *) row, width);
        }

        // Yield periodically so the IDLE task can feed the watchdog
        if ((y & 15) == 0) {
            vTaskDelay(1);
        }
    }

    free((void *) row);
    return valid;
}

esp_err_t image_processor_process_or_display_png(const char *path,
                                                 dither_algorithm_t dither_algorithm,
                                                 const display_publish_t *pub, bool release_source)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }

    // Fused fast path: a pre-processed PNG validates and paints straight
    // from the file in a single decode, with no RAM copy of the upload (a
    // near-5 MB source competes with the frame buffer, and on MemFS the
    // file already lives in PSRAM). Any validation failure leaves the panel
    // untouched and the source falls back to full processing.
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return ESP_FAIL;
    }

    uint8_t sig[8];
    if (fread(sig, 1, 8, fp) == 8 && png_sig_cmp(sig, 0, 8) == 0) {
        png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
        png_infop info_ptr = png_ptr ? png_create_info_struct(png_ptr) : NULL;
        if (png_ptr && info_ptr) {
            png_init_io(png_ptr, fp);
            png_set_sig_bytes(png_ptr, 8);

            esp_err_t err = display_manager_begin_rgb_stream();
            if (err != ESP_OK) {
                png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
                fclose(fp);
                return err;
            }

            bool displayed = check_processed_png(png_ptr, info_ptr, true);
            esp_err_t end_err = display_manager_end_rgb_stream(displayed, pub);
            png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

            if (displayed) {
                fclose(fp);
                ESP_LOGI(TAG, "Displayed pre-processed PNG in a single decode");
                return end_err;
            }
            ESP_LOGI(TAG, "PNG needs processing");
        } else if (png_ptr) {
            png_destroy_read_struct(&png_ptr, NULL, NULL);
        }
    }
    fclose(fp);

    // Fallback: full processing needs the compressed source in RAM
    fp = fopen(path, "rb");
    if (!fp) {
        return ESP_FAIL;
    }
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (file_size <= 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    uint8_t *file_buffer = (uint8_t *) heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!file_buffer) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    size_t read_bytes = fread(file_buffer, 1, file_size, fp);
    fclose(fp);
    if (read_bytes != (size_t) file_size) {
        heap_caps_free(file_buffer);
        return ESP_FAIL;
    }

    if (release_source) {
        // MemFS-backed sources live in PSRAM; drop the file now that the
        // compressed copy exists so the two never coexist with the decoder
        unlink(path);
    }

    esp_err_t err = image_processor_process_to_display(file_buffer, (size_t) file_size,
                                                       IMAGE_FORMAT_PNG, dither_algorithm, pub);
    heap_caps_free(file_buffer);
    return err;
}

image_format_t image_processor_detect_format(const char *input_path)
{
    FILE *fp = fopen(input_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open file for format detection: %s", input_path);
        return IMAGE_FORMAT_UNKNOWN;
    }

    uint8_t magic[8];
    size_t read = fread(magic, 1, 8, fp);
    fclose(fp);

    if (read < 2) {
        return IMAGE_FORMAT_UNKNOWN;
    }

    if (read >= 8 && magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47 &&
        magic[4] == 0x0D && magic[5] == 0x0A && magic[6] == 0x1A && magic[7] == 0x0A) {
        return IMAGE_FORMAT_PNG;
    } else if (magic[0] == 0x42 && magic[1] == 0x4D) {
        return IMAGE_FORMAT_BMP;
    } else if (magic[0] == 0xFF && magic[1] == 0xD8) {
        return IMAGE_FORMAT_JPG;
    } else if (magic[0] == 0x1F && magic[1] == 0x8B) {
        return IMAGE_FORMAT_EPD_GZ;
    }

    return IMAGE_FORMAT_UNKNOWN;
}
