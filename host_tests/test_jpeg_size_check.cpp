// The size check of esp_jpeg's answer (main/jpeg_size_check.h).

#include <gtest/gtest.h>

#include <cstdint>

extern "C" {
#include "jpeg_size_check.h"
}

TEST(JpegSizeCheck, TheSizeOfAnOrdinaryPictureIsAccepted)
{
    EXPECT_TRUE(jpeg_output_size_ok(800, 480, 0, 800 * 480 * 3));
    EXPECT_TRUE(jpeg_output_size_ok(4032, 3024, 0, 4032 * 3024 * 3));  // a phone photo
    EXPECT_TRUE(jpeg_output_size_ok(12000, 9000, 0,
                                    12000u * 9000u * 3u));  // 108 megapixels, 324 MB: no wrap
}

TEST(JpegSizeCheck, EachScaleDividesEachSideSeparately)
{
    EXPECT_TRUE(jpeg_output_size_ok(800, 480, 1, 400 * 240 * 3));
    EXPECT_TRUE(jpeg_output_size_ok(
        801, 481, 1, 400 * 240 * 3));  // the division of each side drops the odd pixel
    EXPECT_TRUE(jpeg_output_size_ok(801, 481, 2, 200 * 120 * 3));
    EXPECT_TRUE(jpeg_output_size_ok(4032, 3024, 2, 1008 * 756 * 3));
    EXPECT_TRUE(jpeg_output_size_ok(12000, 9000, 2, 3000 * 2250 * 3));
    EXPECT_TRUE(jpeg_output_size_ok(803, 483, 3, 100 * 60 * 3));
}

TEST(JpegSizeCheck, ASizeThatWrapsIn32BitIsRefused)
{
    // 40000 x 35792 pixels: (uint32_t)(35792 * 40000 * 3) wraps to 72704, but the real size is
    // 4295040000 - the case this check exists for. A caller that (unlike esp_jpeg) reports the
    // real, un-wrapped size is accepted.
    EXPECT_FALSE(jpeg_output_size_ok(40000, 35792, 0, 72704));
    EXPECT_TRUE(jpeg_output_size_ok(40000, 35792, 0, (size_t) 40000 * 35792 * 3));
}

TEST(JpegSizeCheck, BadSidesOrScalesAreRefused)
{
    EXPECT_FALSE(jpeg_output_size_ok(0, 480, 0, 0));
    EXPECT_FALSE(jpeg_output_size_ok(800, 0, 0, 0));
    EXPECT_FALSE(jpeg_output_size_ok(-1, 480, 0, 800 * 480 * 3));
    EXPECT_FALSE(jpeg_output_size_ok(800, 480, -1, 800 * 480 * 3));
    EXPECT_FALSE(jpeg_output_size_ok(800, 480, 4, 800 * 480 * 3));
    EXPECT_FALSE(jpeg_output_size_ok(800, 480, 0, 800 * 480 * 3 - 1));
    EXPECT_FALSE(jpeg_output_size_ok(800, 480, 0, 800 * 480 * 3 + 1));
}
