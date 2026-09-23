#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include "chime_name.h"
#include "settings_backup.h"
}

namespace
{

settings_backup_t sample()
{
    settings_backup_t s = {};
    s.auto_rotate = true;
    s.has_auto_rotate = true;
    s.rotate_cron_count = 2;
    strncpy(s.rotate_cron[0], "0 */12 *", sizeof(s.rotate_cron[0]) - 1);
    strncpy(s.rotate_cron[1], "*/30 8-22 1-5", sizeof(s.rotate_cron[1]) - 1);
    s.has_rotate_cron = true;
    strncpy(s.rotation_mode, "url", sizeof(s.rotation_mode) - 1);
    s.has_rotation_mode = true;
    strncpy(s.sd_rotation_mode, "sequential", sizeof(s.sd_rotation_mode) - 1);
    s.has_sd_rotation_mode = true;
    strncpy(s.image_url, "http://news.local:9607/image/immich", sizeof(s.image_url) - 1);
    s.has_image_url = true;
    strncpy(s.ha_url, "http://ha.local:8123", sizeof(s.ha_url) - 1);
    s.has_ha_url = true;
    strncpy(s.timezone, "CET-1CEST,M3.5.0/2,M10.5.0/3", sizeof(s.timezone) - 1);
    s.has_timezone = true;
    strncpy(s.ntp_server, "nl.pool.ntp.org", sizeof(s.ntp_server) - 1);
    s.has_ntp_server = true;
    strncpy(s.device_name, "Woonkamer", sizeof(s.device_name) - 1);
    s.has_device_name = true;
    strncpy(s.display_orientation, "portrait", sizeof(s.display_orientation) - 1);
    s.has_display_orientation = true;
    s.display_rotation_deg = 0;
    s.has_display_rotation_deg = true;
    s.save_downloaded_images = true;
    s.has_save_downloaded_images = true;
    s.debug_log_enabled = true;
    s.has_debug_log_enabled = true;
    s.deep_sleep_enabled = false;
    s.has_deep_sleep_enabled = true;
    s.chime_enabled = true;
    s.has_chime_enabled = true;
    strncpy(s.chime_preset, "dingdong", sizeof(s.chime_preset) - 1);
    s.has_chime_preset = true;
    strncpy(s.chime_url, "http://news.local:8080/chime.wav", sizeof(s.chime_url) - 1);
    s.has_chime_url = true;
    strncpy(s.chime_source, "wav", sizeof(s.chime_source) - 1);
    s.has_chime_source = true;
    strncpy(s.chime_pull_mode, "with_rotate", sizeof(s.chime_pull_mode) - 1);
    s.has_chime_pull_mode = true;
    strncpy(s.chime_play_when, "before", sizeof(s.chime_play_when) - 1);
    s.has_chime_play_when = true;
    strncpy(s.chime_file, "doorbell.wav", sizeof(s.chime_file) - 1);
    s.has_chime_file = true;
    return s;
}

}  // namespace

TEST(SettingsBackup, PathMatchesConfigFolder)
{
    EXPECT_STREQ(SETTINGS_BACKUP_PATH, "/storage/config/settings.json");
}

TEST(SettingsBackup, RestoreOnlyWhenNvsIsFactoryFresh)
{
    EXPECT_TRUE(settings_backup_should_restore(false, true));
    EXPECT_FALSE(settings_backup_should_restore(true, true));
    EXPECT_FALSE(settings_backup_should_restore(false, false));
    EXPECT_FALSE(settings_backup_should_restore(true, false));
}

TEST(SettingsBackup, RoundTripPreservesListedKeys)
{
    settings_backup_t in = sample();
    char buf[4096];
    ASSERT_GT(settings_backup_serialize(&in, buf, sizeof(buf)), 0);

    settings_backup_t out = {};
    ASSERT_TRUE(settings_backup_parse(buf, &out));

    EXPECT_TRUE(out.has_auto_rotate);
    EXPECT_TRUE(out.auto_rotate);
    ASSERT_EQ(out.rotate_cron_count, 2);
    EXPECT_STREQ(out.rotate_cron[0], "0 */12 *");
    EXPECT_STREQ(out.rotate_cron[1], "*/30 8-22 1-5");
    EXPECT_STREQ(out.rotation_mode, "url");
    EXPECT_STREQ(out.sd_rotation_mode, "sequential");
    EXPECT_STREQ(out.image_url, "http://news.local:9607/image/immich");
    EXPECT_STREQ(out.ha_url, "http://ha.local:8123");
    EXPECT_TRUE(out.has_timezone);
    EXPECT_STREQ(out.timezone, "CET-1CEST,M3.5.0/2,M10.5.0/3");
    EXPECT_STREQ(out.ntp_server, "nl.pool.ntp.org");
    EXPECT_STREQ(out.device_name, "Woonkamer");
    EXPECT_STREQ(out.display_orientation, "portrait");
    EXPECT_EQ(out.display_rotation_deg, 0);
    EXPECT_TRUE(out.save_downloaded_images);
    EXPECT_TRUE(out.debug_log_enabled);
    EXPECT_TRUE(out.has_deep_sleep_enabled);
    EXPECT_FALSE(out.deep_sleep_enabled);
    EXPECT_TRUE(out.chime_enabled);
    EXPECT_STREQ(out.chime_preset, "dingdong");
    EXPECT_STREQ(out.chime_url, "http://news.local:8080/chime.wav");
    EXPECT_STREQ(out.chime_source, "wav");
    EXPECT_STREQ(out.chime_pull_mode, "with_rotate");
    EXPECT_STREQ(out.chime_play_when, "before");
    EXPECT_STREQ(out.chime_file, "doorbell.wav");
}

TEST(SettingsBackup, TimezonePosixRoundTrip)
{
    settings_backup_t in = {};
    strncpy(in.timezone, "CET-1CEST,M3.5.0/2,M10.5.0/3", sizeof(in.timezone) - 1);
    in.has_timezone = true;

    char buf[4096];
    ASSERT_GT(settings_backup_serialize(&in, buf, sizeof(buf)), 0);
    EXPECT_NE(std::string(buf).find("\"timezone\": \"CET-1CEST,M3.5.0/2,M10.5.0/3\""),
              std::string::npos);
    EXPECT_NE(std::string(buf).find("\"version\": 2"), std::string::npos);

    settings_backup_t out = {};
    ASSERT_TRUE(settings_backup_parse(buf, &out));
    EXPECT_TRUE(out.has_timezone);
    EXPECT_STREQ(out.timezone, "CET-1CEST,M3.5.0/2,M10.5.0/3");
}

TEST(SettingsBackup, V1SnapshotWithoutTimezoneStillParses)
{
    const char *json =
        "{\"version\":1,\"auto_rotate\":true,\"rotate_cron\":[\"0 */12 *\"],"
        "\"rotation_mode\":\"url\",\"image_url\":\"http://x\","
        "\"deep_sleep_enabled\":false,\"chime_enabled\":true,"
        "\"chime_preset\":\"mozart\",\"chime_url\":\"\","
        "\"chime_source\":\"preset\",\"chime_pull_mode\":\"with_rotate\","
        "\"chime_play_when\":\"after\",\"chime_file\":\"\"}";

    settings_backup_t out = {};
    ASSERT_TRUE(settings_backup_parse(json, &out));
    EXPECT_TRUE(out.auto_rotate);
    EXPECT_FALSE(out.has_timezone);
    EXPECT_STREQ(out.timezone, "");
    EXPECT_FALSE(out.has_ntp_server);
    EXPECT_FALSE(out.has_device_name);
}

TEST(SettingsBackup, ParsesCompactAndEscapedJson)
{
    const char *json =
        "{\"version\":1,\"auto_rotate\":false,\"rotate_cron\":[\"0 8 *\"],"
        "\"rotation_mode\":\"storage\",\"image_url\":\"http://x/\\\"y\\\"\","
        "\"deep_sleep_enabled\":true,\"chime_enabled\":false,"
        "\"chime_preset\":\"softping\",\"chime_url\":\"\","
        "\"chime_source\":\"uploaded\",\"chime_pull_mode\":\"once\","
        "\"chime_play_when\":\"after\",\"chime_file\":\"bell.wav\"}";

    settings_backup_t out = {};
    ASSERT_TRUE(settings_backup_parse(json, &out));
    EXPECT_FALSE(out.auto_rotate);
    ASSERT_EQ(out.rotate_cron_count, 1);
    EXPECT_STREQ(out.rotate_cron[0], "0 8 *");
    EXPECT_STREQ(out.rotation_mode, "storage");
    EXPECT_STREQ(out.image_url, "http://x/\"y\"");
    EXPECT_TRUE(out.deep_sleep_enabled);
    EXPECT_FALSE(out.chime_enabled);
    EXPECT_STREQ(out.chime_preset, "softping");
    EXPECT_STREQ(out.chime_source, "uploaded");
    EXPECT_STREQ(out.chime_play_when, "after");
    EXPECT_STREQ(out.chime_file, "bell.wav");
}

TEST(SettingsBackup, AcceptsNewTunePresetIds)
{
    const char *json =
        "{\"chime_preset\":\"mozart\",\"chime_source\":\"preset\","
        "\"chime_file\":\"bell.wav\"}";

    settings_backup_t out = {};
    ASSERT_TRUE(settings_backup_parse(json, &out));
    EXPECT_TRUE(out.has_chime_preset);
    EXPECT_STREQ(out.chime_preset, "mozart");
}

TEST(SettingsBackup, AcceptsSdcardAliasAndSkipsInvalidEnums)
{
    const char *json =
        "{\"rotation_mode\":\"sdcard\",\"chime_preset\":\"not-a-tone\","
        "\"chime_source\":\"bogus\",\"chime_file\":\"../evil.wav\","
        "\"chime_play_when\":\"during\",\"chime_pull_mode\":\"always\","
        "\"image_url\":\"http://ok.example/img\",\"auto_rotate\":true}";

    settings_backup_t out = {};
    ASSERT_TRUE(settings_backup_parse(json, &out));
    EXPECT_TRUE(out.has_rotation_mode);
    EXPECT_STREQ(out.rotation_mode, "storage");
    EXPECT_FALSE(out.has_chime_preset);
    EXPECT_FALSE(out.has_chime_source);
    EXPECT_FALSE(out.has_chime_file);
    EXPECT_FALSE(out.has_chime_play_when);
    EXPECT_FALSE(out.has_chime_pull_mode);
    EXPECT_TRUE(out.has_image_url);
    EXPECT_STREQ(out.image_url, "http://ok.example/img");
    EXPECT_TRUE(out.auto_rotate);
}

TEST(SettingsBackup, IgnoresUnknownKeys)
{
    const char *json =
        "{\"version\":1,\"future_flag\":true,\"nested\":{\"x\":1},"
        "\"auto_rotate\":true,\"image_url\":\"http://a\"}";

    settings_backup_t out = {};
    ASSERT_TRUE(settings_backup_parse(json, &out));
    EXPECT_TRUE(out.auto_rotate);
    EXPECT_STREQ(out.image_url, "http://a");
}

TEST(SettingsBackup, RejectsMalformedJson)
{
    settings_backup_t out = {};
    EXPECT_FALSE(settings_backup_parse("not json", &out));
    EXPECT_FALSE(settings_backup_parse("[1,2]", &out));
    EXPECT_FALSE(settings_backup_parse(NULL, &out));
}

TEST(SettingsBackup, SerializeFailsOnTinyBuffer)
{
    settings_backup_t in = sample();
    char tiny[8];
    EXPECT_EQ(settings_backup_serialize(&in, tiny, sizeof(tiny)), -1);
}

TEST(SettingsBackup, SerializeDefaultsPullModeWithRotateAndPlayWhenAfter)
{
    settings_backup_t in = {};
    char buf[4096];
    ASSERT_GT(settings_backup_serialize(&in, buf, sizeof(buf)), 0);
    EXPECT_NE(std::string(buf).find("\"chime_pull_mode\": \"with_rotate\""), std::string::npos);
    EXPECT_NE(std::string(buf).find("\"chime_play_when\": \"after\""), std::string::npos);
}

TEST(SettingsBackup, RejectsInvalidDisplayEnumsAndKeepsValidTimezone)
{
    const char *json =
        "{\"timezone\":\"UTC-1\",\"display_orientation\":\"upside-down\","
        "\"display_rotation_deg\":90,\"sd_rotation_mode\":\"shuffle\","
        "\"ntp_server\":\"pool.ntp.org\"}";

    settings_backup_t out = {};
    ASSERT_TRUE(settings_backup_parse(json, &out));
    EXPECT_TRUE(out.has_timezone);
    EXPECT_STREQ(out.timezone, "UTC-1");
    EXPECT_FALSE(out.has_display_orientation);
    EXPECT_FALSE(out.has_display_rotation_deg);
    EXPECT_FALSE(out.has_sd_rotation_mode);
    EXPECT_TRUE(out.has_ntp_server);
    EXPECT_STREQ(out.ntp_server, "pool.ntp.org");
}

TEST(SettingsBackup, AcceptsPlayWhenBeforeAndAfter)
{
    const char *json = "{\"chime_play_when\":\"before\",\"chime_pull_mode\":\"with_rotate\"}";
    settings_backup_t out = {};
    ASSERT_TRUE(settings_backup_parse(json, &out));
    EXPECT_TRUE(out.has_chime_play_when);
    EXPECT_STREQ(out.chime_play_when, "before");
    EXPECT_TRUE(out.has_chime_pull_mode);
    EXPECT_STREQ(out.chime_pull_mode, "with_rotate");
}
