#include <gtest/gtest.h>

extern "C" {
#include "chime_policy.h"
}

TEST(ChimePolicy, PreviewFetchesWhenCacheMissingOrRefresh)
{
    EXPECT_TRUE(chime_should_fetch_url(CHIME_PLAY_PREVIEW, false, false, false));
    EXPECT_TRUE(chime_should_fetch_url(CHIME_PLAY_PREVIEW, false, true, true));
    EXPECT_FALSE(chime_should_fetch_url(CHIME_PLAY_PREVIEW, false, true, false));
    EXPECT_FALSE(chime_should_fetch_url(CHIME_PLAY_PREVIEW, true, true, false));
}

TEST(ChimePolicy, AfterDisplayRespectsPullMode)
{
    EXPECT_TRUE(chime_should_fetch_url(CHIME_PLAY_AFTER_DISPLAY, true, true, false));
    EXPECT_TRUE(chime_should_fetch_url(CHIME_PLAY_AFTER_DISPLAY, false, false, false));
    EXPECT_FALSE(chime_should_fetch_url(CHIME_PLAY_AFTER_DISPLAY, false, true, false));
}

TEST(ChimePolicy, PlayedNameMatchesApi)
{
    EXPECT_STREQ(chime_played_name(CHIME_PLAYED_WAV), "wav");
    EXPECT_STREQ(chime_played_name(CHIME_PLAYED_PRESET), "preset");
    EXPECT_STREQ(chime_played_name(CHIME_PLAYED_UPLOADED), "uploaded");
    EXPECT_STREQ(chime_played_name(CHIME_PLAYED_NONE), "none");
}
