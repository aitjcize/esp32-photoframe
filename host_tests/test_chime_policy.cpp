#include <gtest/gtest.h>

extern "C" {
#include "chime_policy.h"
}

TEST(ChimePolicy, DefaultsAreWithRotateAndAfter)
{
    EXPECT_STREQ(CHIME_PULL_MODE_DEFAULT, "with_rotate");
    EXPECT_STREQ(CHIME_PLAY_WHEN_DEFAULT, "after");
}

TEST(ChimePolicy, PlayWhenValidation)
{
    EXPECT_TRUE(chime_play_when_is_valid("after"));
    EXPECT_TRUE(chime_play_when_is_valid("before"));
    EXPECT_FALSE(chime_play_when_is_valid(nullptr));
    EXPECT_FALSE(chime_play_when_is_valid(""));
    EXPECT_FALSE(chime_play_when_is_valid("during"));
    EXPECT_FALSE(chime_play_when_is_valid("AFTER"));
}

TEST(ChimePolicy, PlayWhenUnknownDefaultsToAfter)
{
    EXPECT_FALSE(chime_play_when_is_before(nullptr));
    EXPECT_FALSE(chime_play_when_is_before(""));
    EXPECT_FALSE(chime_play_when_is_before("after"));
    EXPECT_FALSE(chime_play_when_is_before("during"));
    EXPECT_TRUE(chime_play_when_is_before("before"));
}

TEST(ChimePolicy, AfterPlaysOnlyWhenRefreshCompletes)
{
    EXPECT_FALSE(chime_should_play_at_hook("after", CHIME_DISPLAY_HOOK_BEFORE_REFRESH));
    EXPECT_TRUE(chime_should_play_at_hook("after", CHIME_DISPLAY_HOOK_AFTER_REFRESH));
    // Default / invalid values must not overlap a long e-ink draw.
    EXPECT_FALSE(chime_should_play_at_hook(nullptr, CHIME_DISPLAY_HOOK_BEFORE_REFRESH));
    EXPECT_TRUE(chime_should_play_at_hook(nullptr, CHIME_DISPLAY_HOOK_AFTER_REFRESH));
    EXPECT_FALSE(chime_should_play_at_hook("", CHIME_DISPLAY_HOOK_BEFORE_REFRESH));
    EXPECT_TRUE(chime_should_play_at_hook("", CHIME_DISPLAY_HOOK_AFTER_REFRESH));
}

TEST(ChimePolicy, BeforePlaysWhenRotateStartsBeforePanelWait)
{
    EXPECT_TRUE(chime_should_play_at_hook("before", CHIME_DISPLAY_HOOK_BEFORE_REFRESH));
    EXPECT_FALSE(chime_should_play_at_hook("before", CHIME_DISPLAY_HOOK_AFTER_REFRESH));
}

TEST(ChimePolicy, PreviewFetchesWhenCacheMissingOrRefresh)
{
    EXPECT_TRUE(chime_should_fetch_url(CHIME_PLAY_PREVIEW, false, false, false));
    EXPECT_TRUE(chime_should_fetch_url(CHIME_PLAY_PREVIEW, false, true, true));
    EXPECT_FALSE(chime_should_fetch_url(CHIME_PLAY_PREVIEW, false, true, false));
    EXPECT_FALSE(chime_should_fetch_url(CHIME_PLAY_PREVIEW, true, true, false));
}

TEST(ChimePolicy, DisplayRotateRespectsPullModeBeforeAndAfter)
{
    EXPECT_TRUE(chime_reason_is_display_rotate(CHIME_PLAY_AFTER_DISPLAY));
    EXPECT_TRUE(chime_reason_is_display_rotate(CHIME_PLAY_BEFORE_DISPLAY));
    EXPECT_FALSE(chime_reason_is_display_rotate(CHIME_PLAY_PREVIEW));

    EXPECT_TRUE(chime_should_fetch_url(CHIME_PLAY_AFTER_DISPLAY, true, true, false));
    EXPECT_TRUE(chime_should_fetch_url(CHIME_PLAY_BEFORE_DISPLAY, true, true, false));
    EXPECT_TRUE(chime_should_fetch_url(CHIME_PLAY_AFTER_DISPLAY, false, false, false));
    EXPECT_TRUE(chime_should_fetch_url(CHIME_PLAY_BEFORE_DISPLAY, false, false, false));
    EXPECT_FALSE(chime_should_fetch_url(CHIME_PLAY_AFTER_DISPLAY, false, true, false));
    EXPECT_FALSE(chime_should_fetch_url(CHIME_PLAY_BEFORE_DISPLAY, false, true, false));
}

TEST(ChimePolicy, PlayedNameMatchesApi)
{
    EXPECT_STREQ(chime_played_name(CHIME_PLAYED_WAV), "wav");
    EXPECT_STREQ(chime_played_name(CHIME_PLAYED_PRESET), "preset");
    EXPECT_STREQ(chime_played_name(CHIME_PLAYED_UPLOADED), "uploaded");
    EXPECT_STREQ(chime_played_name(CHIME_PLAYED_NONE), "none");
}
