// Characterization tests for the on-device image pipeline.
//
// The assertions were pinned against the pre-row-streaming baseline
// (3ab0be3) and carried forward across the streaming rewrite. Where the
// rewrite intentionally changed behavior, the updated expectation is noted
// with a CHANGED comment:
//   - rotation follows the configured display orientation, not the image's
//     aspect ratio vs the panel
//   - aspect-mismatched sources are cover-cropped instead of stretched
//   - pre-processed PNG detection became validate-while-painting
//     (image_processor_process_or_display_png)
//   - GC16 grayscale panels got their own decode/dither path (new tests)

#include <gtest/gtest.h>
#include <png.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

extern "C" {
#include "config_manager.h"
#include "fake_display_manager.h"
#include "image_processor.h"
#include "processing_settings.h"

extern int test_board_display_width;
extern int test_board_display_height;
extern const char *test_board_display_type;
extern display_orientation_t test_display_orientation;
extern scale_mode_t test_scale_mode;
extern const char *test_background_color;
}

namespace
{

struct Rgb {
    uint8_t r, g, b;
    bool operator==(const Rgb &o) const
    {
        return r == o.r && g == o.g && b == o.b;
    }
    bool operator!=(const Rgb &o) const
    {
        return !(*this == o);
    }
    bool operator<(const Rgb &o) const
    {
        return std::tie(r, g, b) < std::tie(o.r, o.g, o.b);
    }
};

std::ostream &operator<<(std::ostream &os, const Rgb &c)
{
    return os << "(" << int(c.r) << "," << int(c.g) << "," << int(c.b) << ")";
}

constexpr Rgb kBlack{0, 0, 0};
constexpr Rgb kWhite{255, 255, 255};
constexpr Rgb kYellow{255, 255, 0};
constexpr Rgb kRed{255, 0, 0};
constexpr Rgb kBlue{0, 0, 255};
constexpr Rgb kGreen{0, 255, 0};

bool InSpectraPalette(const Rgb &c)
{
    return c == kBlack || c == kWhite || c == kYellow || c == kRed || c == kBlue || c == kGreen;
}

// GC16 output pixels are gray ramp levels: r == g == b, multiple of 17
bool InGc16Palette(const Rgb &c)
{
    return c.r == c.g && c.g == c.b && c.r % 17 == 0;
}

// Encode an RGB(A) image as a PNG in memory using a per-pixel generator.
using PixelFn = std::function<Rgb(int x, int y)>;

std::vector<uint8_t> EncodePng(int w, int h, const PixelFn &pixel, bool with_alpha = false)
{
    std::vector<uint8_t> out;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return {};
    }
    png_set_write_fn(
        png, &out,
        [](png_structp p, png_bytep data, png_size_t len) {
            auto *vec = static_cast<std::vector<uint8_t> *>(png_get_io_ptr(p));
            vec->insert(vec->end(), data, data + len);
        },
        nullptr);
    png_set_IHDR(png, info, w, h, 8, with_alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    const int ch = with_alpha ? 4 : 3;
    std::vector<uint8_t> row(static_cast<size_t>(w) * ch);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            Rgb c = pixel(x, y);
            row[x * ch] = c.r;
            row[x * ch + 1] = c.g;
            row[x * ch + 2] = c.b;
            if (with_alpha)
                row[x * ch + 3] = 255;
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    return out;
}

// The frame the display would show, captured from the fake display_manager.
struct Processed {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;

    Rgb at(int x, int y) const
    {
        size_t i = (static_cast<size_t>(y) * w + x) * 3;
        return Rgb{rgb[i], rgb[i + 1], rgb[i + 2]};
    }

    // Most frequent color in a block, for sampling solid regions.
    Rgb dominant(int x0, int y0, int bw, int bh) const
    {
        std::map<Rgb, int> counts;
        for (int y = y0; y < y0 + bh; y++)
            for (int x = x0; x < x0 + bw; x++)
                counts[at(x, y)]++;
        Rgb best{};
        int best_n = -1;
        for (const auto &kv : counts)
            if (kv.second > best_n) {
                best = kv.first;
                best_n = kv.second;
            }
        return best;
    }

    double fraction(const Rgb &c) const
    {
        size_t n = 0, total = static_cast<size_t>(w) * h;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (at(x, y) == c)
                    n++;
        return total ? double(n) / double(total) : 0.0;
    }

    bool allInPalette(bool (*member)(const Rgb &) = InSpectraPalette) const
    {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (!member(at(x, y)))
                    return false;
        return true;
    }

    double meanChannel(int c) const
    {
        double sum = 0;
        size_t total = static_cast<size_t>(w) * h;
        for (size_t i = 0; i < total; i++)
            sum += rgb[i * 3 + c];
        return sum / double(total);
    }

    // Number of pixels that differ from the generator's expectation.
    size_t mismatches(const PixelFn &pixel) const
    {
        size_t n = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (at(x, y) != pixel(x, y))
                    n++;
        return n;
    }
};

Processed CaptureFrame()
{
    Processed p;
    p.w = fake_display_frame_width();
    p.h = fake_display_frame_height();
    const uint8_t *f = fake_display_frame();
    EXPECT_NE(f, nullptr) << "nothing was streamed to the display";
    if (f)
        p.rgb.assign(f, f + static_cast<size_t>(p.w) * p.h * 3);
    return p;
}

// Pipeline entry point: buffer in, displayed frame out.
Processed RunPipeline(const std::vector<uint8_t> &png)
{
    esp_err_t err = image_processor_process_to_display(png.data(), png.size(), IMAGE_FORMAT_PNG,
                                                       DITHER_FLOYD_STEINBERG, nullptr);
    EXPECT_EQ(err, ESP_OK) << "pipeline failed: " << image_processor_get_last_error();
    if (err != ESP_OK)
        return {};
    EXPECT_TRUE(fake_display_was_shown());
    return CaptureFrame();
}

// File entry point used by the PNG display fast path.
Processed RunPngFile(const std::vector<uint8_t> &png)
{
    std::string path = ::testing::TempDir() + "pipeline_test.png";
    FILE *fp = fopen(path.c_str(), "wb");
    EXPECT_NE(fp, nullptr);
    fwrite(png.data(), 1, png.size(), fp);
    fclose(fp);
    esp_err_t err = image_processor_process_or_display_png(path.c_str(), DITHER_FLOYD_STEINBERG,
                                                           nullptr, false);
    remove(path.c_str());
    EXPECT_EQ(err, ESP_OK) << "display failed: " << image_processor_get_last_error();
    if (err != ESP_OK)
        return {};
    return CaptureFrame();
}

class ImagePipelineTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        test_board_display_width = 800;
        test_board_display_height = 480;
        test_board_display_type = "spectra6";
        test_display_orientation = DISPLAY_ORIENTATION_LANDSCAPE;
        test_scale_mode = SCALE_MODE_COVER;
        test_background_color = "white";
        fake_display_reset();
        ASSERT_EQ(image_processor_init(), ESP_OK);
    }
};

// --- Geometry -------------------------------------------------------------

TEST_F(ImagePipelineTest, LandscapeInputScalesToPanel)
{
    // Same aspect as the panel so scaling has a unique right answer.
    auto png = EncodePng(1600, 960,
                         [](int x, int y) { return Rgb{uint8_t(x % 256), uint8_t(y % 256), 128}; });
    Processed p = RunPipeline(png);
    EXPECT_EQ(p.w, 800);
    EXPECT_EQ(p.h, 480);
    EXPECT_TRUE(p.allInPalette());
}

// CHANGED: with a landscape orientation configured, a portrait source is
// cover-cropped to the panel; the baseline rotated it by aspect instead.
TEST_F(ImagePipelineTest, PortraitInputCoverCropsToPanel)
{
    auto png = EncodePng(960, 1600,
                         [](int x, int y) { return Rgb{uint8_t(x % 256), uint8_t(y % 256), 128}; });
    Processed p = RunPipeline(png);
    EXPECT_EQ(p.w, 800);
    EXPECT_EQ(p.h, 480);
}

// CHANGED: rotation is driven by the configured orientation. Landscape
// orientation on a native-portrait panel streams rotated output.
TEST_F(ImagePipelineTest, LandscapeOrientationOnPortraitPanelRotates)
{
    test_board_display_width = 480;
    test_board_display_height = 800;
    auto png = EncodePng(1600, 960,
                         [](int x, int y) { return Rgb{uint8_t(x % 256), uint8_t(y % 256), 128}; });
    Processed p = RunPipeline(png);
    EXPECT_EQ(p.w, 480);
    EXPECT_EQ(p.h, 800);
}

// CHANGED: cover-crop instead of stretch; the panel is still filled.
TEST_F(ImagePipelineTest, AspectMismatchStillFillsPanel)
{
    auto png = EncodePng(1000, 900, [](int, int) { return kWhite; });
    Processed p = RunPipeline(png);
    EXPECT_EQ(p.w, 800);
    EXPECT_EQ(p.h, 480);
    EXPECT_GT(p.fraction(kWhite), 0.999);
}

// The four orientation-config x panel-shape combinations, each verified by
// quadrant mapping: content must render in the orientation the user
// configured, not whatever the source or panel aspect suggests.

TEST_F(ImagePipelineTest, LandscapeOrientationOnLandscapePanelKeepsUpright)
{
    auto png = EncodePng(800, 480, [](int x, int y) {
        if (y < 240)
            return x < 400 ? kWhite : kRed;  // top-left white, top-right red
        return x < 400 ? kBlue : kBlack;     // bottom-left blue, bottom-right black
    });
    Processed p = RunPipeline(png);
    ASSERT_EQ(p.w, 800);
    ASSERT_EQ(p.h, 480);
    EXPECT_EQ(p.dominant(40, 40, 40, 40), kWhite) << "top-left";
    EXPECT_EQ(p.dominant(720, 40, 40, 40), kRed) << "top-right";
    EXPECT_EQ(p.dominant(40, 400, 40, 40), kBlue) << "bottom-left";
    EXPECT_EQ(p.dominant(720, 400, 40, 40), kBlack) << "bottom-right";
}

TEST_F(ImagePipelineTest, PortraitOrientationOnPortraitPanelKeepsUpright)
{
    test_board_display_width = 480;
    test_board_display_height = 800;
    test_display_orientation = DISPLAY_ORIENTATION_PORTRAIT;
    auto png = EncodePng(480, 800, [](int x, int y) {
        if (y < 400)
            return x < 240 ? kWhite : kRed;
        return x < 240 ? kBlue : kBlack;
    });
    Processed p = RunPipeline(png);
    ASSERT_EQ(p.w, 480);
    ASSERT_EQ(p.h, 800);
    EXPECT_EQ(p.dominant(40, 40, 40, 40), kWhite) << "top-left";
    EXPECT_EQ(p.dominant(400, 40, 40, 40), kRed) << "top-right";
    EXPECT_EQ(p.dominant(40, 720, 40, 40), kBlue) << "bottom-left";
    EXPECT_EQ(p.dominant(400, 720, 40, 40), kBlack) << "bottom-right";
}

TEST_F(ImagePipelineTest, LandscapeOrientationOnPortraitPanelRotatesClockwise)
{
    test_board_display_width = 480;
    test_board_display_height = 800;
    // Orientation stays LANDSCAPE: processing space is 800x480, streamed
    // rotated onto the native-portrait panel
    auto png = EncodePng(800, 480, [](int x, int y) {
        if (y < 240)
            return x < 400 ? kWhite : kRed;
        return x < 400 ? kBlue : kBlack;
    });
    Processed p = RunPipeline(png);
    ASSERT_EQ(p.w, 480);
    ASSERT_EQ(p.h, 800);
    // Same clockwise mapping as portrait-on-landscape: processing top edge
    // lands on the panel's right edge
    EXPECT_EQ(p.dominant(40, 40, 40, 40), kBlue) << "top-left";
    EXPECT_EQ(p.dominant(400, 40, 40, 40), kWhite) << "top-right";
    EXPECT_EQ(p.dominant(40, 720, 40, 40), kBlack) << "bottom-left";
    EXPECT_EQ(p.dominant(400, 720, 40, 40), kRed) << "bottom-right";
}

// Portrait orientation on a native-landscape panel: the panel image must be
// the processing-space (portrait) image rotated 90 degrees clockwise, same
// direction as the baseline's aspect-based rotation.
TEST_F(ImagePipelineTest, PortraitOrientationRotationIsClockwise)
{
    test_display_orientation = DISPLAY_ORIENTATION_PORTRAIT;
    auto png = EncodePng(480, 800, [](int x, int y) {
        if (y < 400)
            return x < 240 ? kWhite : kRed;  // top-left white, top-right red
        return x < 240 ? kBlue : kBlack;     // bottom-left blue, bottom-right black
    });
    Processed p = RunPipeline(png);
    ASSERT_EQ(p.w, 800);
    ASSERT_EQ(p.h, 480);
    EXPECT_EQ(p.dominant(40, 40, 40, 40), kBlue) << "top-left";
    EXPECT_EQ(p.dominant(720, 40, 40, 40), kWhite) << "top-right";
    EXPECT_EQ(p.dominant(40, 400, 40, 40), kBlack) << "bottom-left";
    EXPECT_EQ(p.dominant(720, 400, 40, 40), kRed) << "bottom-right";
}

// --- Color mapping --------------------------------------------------------

// Per-channel CDR maps pure white exactly onto the measured white point, so
// a white field dithers with zero error: no colored speckle (the earlier
// luminance-based CDR left a bias that dithered into ~1% red dots).
TEST_F(ImagePipelineTest, SolidWhiteStaysWhite)
{
    Processed p = RunPipeline(EncodePng(800, 480, [](int, int) { return kWhite; }));
    EXPECT_GT(p.fraction(kWhite), 0.999);
}

TEST_F(ImagePipelineTest, SolidBlackStaysBlack)
{
    Processed p = RunPipeline(EncodePng(800, 480, [](int, int) { return kBlack; }));
    EXPECT_GT(p.fraction(kBlack), 0.999);
}

TEST_F(ImagePipelineTest, SolidRedMapsToPaletteRed)
{
    Processed p = RunPipeline(EncodePng(800, 480, [](int, int) { return kRed; }));
    EXPECT_GT(p.fraction(kRed), 0.95);
}

TEST_F(ImagePipelineTest, SmallImageUpscales)
{
    Processed p = RunPipeline(EncodePng(100, 60, [](int, int) { return kRed; }));
    EXPECT_EQ(p.w, 800);
    EXPECT_EQ(p.h, 480);
    EXPECT_GT(p.fraction(kRed), 0.95);
}

TEST_F(ImagePipelineTest, PhotoOutputStaysInPalette)
{
    auto png = EncodePng(1600, 960, [](int x, int y) {
        return Rgb{uint8_t((x * 7 + y * 3) % 256), uint8_t((x * 2 + y * 11) % 256),
                   uint8_t((x * 5 + y * 5) % 256)};
    });
    Processed p = RunPipeline(png);
    EXPECT_TRUE(p.allInPalette());
}

TEST_F(ImagePipelineTest, AlphaPngIsAccepted)
{
    auto png = EncodePng(800, 480, [](int, int) { return kWhite; }, /*with_alpha=*/true);
    Processed p = RunPipeline(png);
    EXPECT_EQ(p.w, 800);
    EXPECT_EQ(p.h, 480);
    EXPECT_GT(p.fraction(kWhite), 0.999);
}

// Mid-gray must dither to a stable black/white mixture: pin the output mean
// so the dither/CDR chain can't drift silently. With per-channel CDR the
// firmware and epaper-image-convert run the same algorithm; the tool
// produces 127.58 on this input.
TEST_F(ImagePipelineTest, MidGrayDitherMeanIsStable)
{
    Processed p = RunPipeline(EncodePng(800, 480, [](int, int) { return Rgb{128, 128, 128}; }));
    double mean = (p.meanChannel(0) + p.meanChannel(1) + p.meanChannel(2)) / 3.0;
    RecordProperty("mid_gray_mean", mean);
    printf("[characterize] mid-gray output mean = %.2f\n", mean);
    EXPECT_GT(mean, 123.0);
    EXPECT_LT(mean, 132.0);
}

// --- Pre-processed PNG fast path ------------------------------------------
// CHANGED: is_processed_buffer became validate-while-painting -- a
// pre-processed PNG must render verbatim in a single pass, everything else
// must fall back to full processing.

namespace preprocessed
{

Rgb Checker(int x, int y)
{
    return ((x + y) & 1) ? kWhite : kBlack;
}

}  // namespace preprocessed

TEST_F(ImagePipelineTest, ProcessedCheckerRendersVerbatim)
{
    Processed p = RunPngFile(EncodePng(800, 480, preprocessed::Checker));
    ASSERT_EQ(p.w, 800);
    ASSERT_EQ(p.h, 480);
    EXPECT_EQ(p.mismatches(preprocessed::Checker), 0u);
    EXPECT_EQ(fake_display_begin_count(), 1);
}

TEST_F(ImagePipelineTest, WrongDimensionsFallBackToProcessing)
{
    Processed p = RunPngFile(EncodePng(400, 240, preprocessed::Checker));
    EXPECT_EQ(p.w, 800);
    EXPECT_EQ(p.h, 480);
    EXPECT_TRUE(p.allInPalette());
}

TEST_F(ImagePipelineTest, NonPaletteColorsFallBackToProcessing)
{
    auto png = EncodePng(800, 480,
                         [](int x, int y) { return Rgb{uint8_t(x % 256), uint8_t(y % 256), 128}; });
    Processed p = RunPngFile(png);
    EXPECT_EQ(p.w, 800);
    EXPECT_EQ(p.h, 480);
    EXPECT_TRUE(p.allInPalette());
}

TEST_F(ImagePipelineTest, GarbageInputFails)
{
    std::vector<uint8_t> junk(64, 0xAB);
    esp_err_t err = image_processor_process_to_display(junk.data(), junk.size(), IMAGE_FORMAT_PNG,
                                                       DITHER_FLOYD_STEINBERG, nullptr);
    EXPECT_NE(err, ESP_OK);
}

// --- Fit (letterbox) scale mode -------------------------------------------

TEST_F(ImagePipelineTest, FitModeLetterboxesPortraitSource)
{
    test_scale_mode = SCALE_MODE_FIT;
    // Portrait source on a landscape frame: content centered, white bars on
    // both sides. 240x480 content at scale 1.0 -> bars are x<280 and x>=520.
    auto png = EncodePng(240, 480, [](int, int) { return kRed; });
    Processed p = RunPipeline(png);
    ASSERT_EQ(p.w, 800);
    ASSERT_EQ(p.h, 480);
    // Bars are EXACTLY the background color -- no dither speckle
    for (int y = 0; y < p.h; y++) {
        for (int x = 0; x < 280; x++)
            ASSERT_EQ(p.at(x, y), kWhite) << "left bar at " << x << "," << y;
        for (int x = 520; x < 800; x++)
            ASSERT_EQ(p.at(x, y), kWhite) << "right bar at " << x << "," << y;
    }
    EXPECT_EQ(p.dominant(380, 220, 40, 40), kRed) << "content center";
}

TEST_F(ImagePipelineTest, FitModeBackgroundColorIsConfigurable)
{
    test_scale_mode = SCALE_MODE_FIT;
    test_background_color = "black";
    auto png = EncodePng(240, 480, [](int, int) { return kWhite; });
    Processed p = RunPipeline(png);
    EXPECT_EQ(p.at(10, 240), kBlack) << "left bar";
    EXPECT_EQ(p.at(790, 240), kBlack) << "right bar";
    EXPECT_EQ(p.dominant(380, 220, 40, 40), kWhite) << "content center";
}

TEST_F(ImagePipelineTest, FitModeTopBottomBars)
{
    test_scale_mode = SCALE_MODE_FIT;
    // Very wide source: bars above and below. 800x240 content centered ->
    // bars are y<120 and y>=360.
    auto png = EncodePng(1600, 480, [](int, int) { return kBlue; });
    Processed p = RunPipeline(png);
    for (int x = 0; x < p.w; x += 7) {
        ASSERT_EQ(p.at(x, 60), kWhite) << "top bar";
        ASSERT_EQ(p.at(x, 420), kWhite) << "bottom bar";
    }
    EXPECT_EQ(p.dominant(380, 220, 40, 40), kBlue) << "content center";
}

TEST_F(ImagePipelineTest, FitModeWithPortraitOrientationRotates)
{
    test_scale_mode = SCALE_MODE_FIT;
    test_display_orientation = DISPLAY_ORIENTATION_PORTRAIT;
    // Landscape source in portrait processing space (480x800): fits to
    // 480x240 centered -> processing-space bars above/below become left and
    // right regions after the clockwise rotation onto the 800x480 panel.
    auto png = EncodePng(960, 480, [](int, int) { return kRed; });
    Processed p = RunPipeline(png);
    ASSERT_EQ(p.w, 800);
    ASSERT_EQ(p.h, 480);
    EXPECT_EQ(p.at(100, 240), kWhite) << "bar";
    EXPECT_EQ(p.at(700, 240), kWhite) << "bar";
    EXPECT_EQ(p.dominant(380, 220, 40, 40), kRed) << "content center";
}

TEST_F(ImagePipelineTest, FitModeHighResolutionPortraitStreamsUncorrupted)
{
    test_scale_mode = SCALE_MODE_FIT;
    // A large aspect-mismatched source exercises the streamed PNG decoder's
    // row ring: fit's smaller scale needs more source rows per output row
    // than cover mode, and an undersized ring smears rows into each other.
    // 3000x4000 quadrants -> content 360x480 centered at x [220,580).
    auto png = EncodePng(3000, 4000, [](int x, int y) {
        if (y < 2000)
            return x < 1500 ? kWhite : kRed;
        return x < 1500 ? kBlue : kBlack;
    });
    Processed p = RunPipeline(png);
    ASSERT_EQ(p.w, 800);
    ASSERT_EQ(p.h, 480);
    EXPECT_EQ(p.dominant(260, 80, 40, 40), kWhite) << "content top-left";
    EXPECT_EQ(p.dominant(500, 80, 40, 40), kRed) << "content top-right";
    EXPECT_EQ(p.dominant(260, 360, 40, 40), kBlue) << "content bottom-left";
    EXPECT_EQ(p.dominant(500, 360, 40, 40), kBlack) << "content bottom-right";
    EXPECT_EQ(p.at(100, 240), kWhite) << "left bar";
    EXPECT_EQ(p.at(700, 240), kWhite) << "right bar";
}

TEST_F(ImagePipelineTest, CoverModeIsUnchangedByDefault)
{
    // Default scale mode: the same portrait source cover-crops (no bars)
    auto png = EncodePng(240, 480, [](int, int) { return kRed; });
    Processed p = RunPipeline(png);
    EXPECT_GT(p.fraction(kRed), 0.95);
}

// --- GC16 grayscale panels (new in the streaming rewrite) ------------------

class Gc16PipelineTest : public ImagePipelineTest
{
   protected:
    void SetUp() override
    {
        ImagePipelineTest::SetUp();
        test_board_display_type = "gc16";
        ASSERT_EQ(image_processor_init(), ESP_OK);
    }
};

TEST_F(Gc16PipelineTest, RampRendersVerbatim)
{
    // 16 vertical bands at the theoretical ramp levels i*17
    auto ramp = [](int x, int y) {
        (void) y;
        uint8_t v = uint8_t((x * 16 / 800) * 17);
        return Rgb{v, v, v};
    };
    Processed p = RunPngFile(EncodePng(800, 480, ramp));
    ASSERT_EQ(p.w, 800);
    ASSERT_EQ(p.h, 480);
    EXPECT_EQ(p.mismatches(ramp), 0u);
    EXPECT_EQ(fake_display_begin_count(), 1);
}

TEST_F(Gc16PipelineTest, SolidWhiteStaysWhite)
{
    Processed p = RunPipeline(EncodePng(800, 480, [](int, int) { return kWhite; }));
    EXPECT_GT(p.fraction(kWhite), 0.999);
}

TEST_F(Gc16PipelineTest, SolidBlackStaysBlack)
{
    Processed p = RunPipeline(EncodePng(800, 480, [](int, int) { return kBlack; }));
    EXPECT_GT(p.fraction(kBlack), 0.999);
}

TEST_F(Gc16PipelineTest, RgbDecodesByLuminanceToGrayRamp)
{
    Processed p = RunPipeline(EncodePng(800, 480, [](int, int) { return kRed; }));
    EXPECT_TRUE(p.allInPalette(InGc16Palette));
    // Red is neither black nor white: the result must be a mid-gray mix
    EXPECT_LT(p.fraction(kWhite), 0.9);
    EXPECT_LT(p.fraction(kBlack), 0.9);
}

TEST_F(Gc16PipelineTest, MidGrayStaysInRamp)
{
    Processed p = RunPipeline(EncodePng(800, 480, [](int, int) { return Rgb{128, 128, 128}; }));
    EXPECT_TRUE(p.allInPalette(InGc16Palette));
    double mean = p.meanChannel(0);
    RecordProperty("gc16_mid_gray_mean", mean);
    printf("[characterize] gc16 mid-gray output mean = %.2f\n", mean);
    // 153.00 on both the firmware and epaper-image-convert: the measured
    // gray endpoints are neutral, so per-channel CDR and the old luminance
    // remap coincide for neutral inputs
    EXPECT_GT(mean, 145.0);
    EXPECT_LT(mean, 161.0);
}

TEST_F(Gc16PipelineTest, FitModeBarsAreExactWhite)
{
    test_scale_mode = SCALE_MODE_FIT;
    auto png = EncodePng(240, 480, [](int, int) { return kBlack; });
    Processed p = RunPipeline(png);
    for (int y = 0; y < p.h; y += 7) {
        ASSERT_EQ(p.at(10, y), kWhite) << "left bar";
        ASSERT_EQ(p.at(790, y), kWhite) << "right bar";
    }
    EXPECT_EQ(p.dominant(380, 220, 40, 40), kBlack) << "content center";
}

TEST_F(Gc16PipelineTest, PhotoStaysInGrayRamp)
{
    auto png = EncodePng(1600, 960, [](int x, int y) {
        return Rgb{uint8_t((x * 7 + y * 3) % 256), uint8_t((x * 2 + y * 11) % 256),
                   uint8_t((x * 5 + y * 5) % 256)};
    });
    Processed p = RunPipeline(png);
    EXPECT_TRUE(p.allInPalette(InGc16Palette));
}

}  // namespace
