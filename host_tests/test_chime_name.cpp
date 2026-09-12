#include <gtest/gtest.h>

#include <cstring>

extern "C" {
#include "chime_name.h"
}

TEST(ChimeName, AcceptsSafeWavNames)
{
    EXPECT_TRUE(chime_filename_is_valid("doorbell.wav"));
    EXPECT_TRUE(chime_filename_is_valid("soft-ping_2.wav"));
    EXPECT_TRUE(chime_filename_is_valid("A.wav"));
}

TEST(ChimeName, RejectsUnsafeNames)
{
    EXPECT_FALSE(chime_filename_is_valid(""));
    EXPECT_FALSE(chime_filename_is_valid(".wav"));
    EXPECT_FALSE(chime_filename_is_valid("ding.mp3"));
    EXPECT_FALSE(chime_filename_is_valid("../x.wav"));
    EXPECT_FALSE(chime_filename_is_valid("chimes/x.wav"));
    EXPECT_FALSE(chime_filename_is_valid("has space.wav"));
}

TEST(ChimeName, SanitizesUploadNames)
{
    char out[CHIME_FILENAME_MAX_LEN];
    ASSERT_TRUE(chime_sanitize_filename("My Door Bell.WAV", out, sizeof(out)));
    EXPECT_STREQ(out, "My_Door_Bell.wav");

    ASSERT_TRUE(chime_sanitize_filename("/tmp/foo/bar.wav", out, sizeof(out)));
    EXPECT_STREQ(out, "bar.wav");

    ASSERT_TRUE(chime_sanitize_filename("C:\\sounds\\ding.wav", out, sizeof(out)));
    EXPECT_STREQ(out, "ding.wav");

    ASSERT_TRUE(chime_sanitize_filename("plain", out, sizeof(out)));
    EXPECT_STREQ(out, "plain.wav");

    EXPECT_FALSE(chime_sanitize_filename("...", out, sizeof(out)));
    EXPECT_FALSE(chime_sanitize_filename("", out, sizeof(out)));
}
