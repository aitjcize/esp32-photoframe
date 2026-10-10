// config_track.c: which fields of a config request were ignored (a JSON type the handler does not
// read, a key no handler looked at), with the real cJSON. The handlers of apply_config_from_json()
// are simulated the way utils.c writes them: `item = get(root, "key"); if (item && is_string(item))
// { ... }`.

#include <gtest/gtest.h>

#include <string>
#include <vector>

extern "C" {
#include "cJSON.h"
#include "config_track.h"
}

namespace
{

struct Parsed {
    cJSON *root;
    explicit Parsed(const char *text) : root(cJSON_Parse(text)) {}
    ~Parsed()
    {
        cJSON_Delete(root);
    }
};

std::vector<std::string> list(cJSON *report, const char *name)
{
    std::vector<std::string> out;
    cJSON *array = report ? cJSON_GetObjectItem(report, name) : nullptr;
    cJSON *entry;
    cJSON_ArrayForEach(entry, array)
    {
        out.push_back(entry->valuestring);
    }
    return out;
}

using V = std::vector<std::string>;

// the pattern of utils.c: a string field, a number field, a bool field
void take_string(cJSON *root, const char *key)
{
    cJSON *item = config_track_get(root, key);
    if (item && config_track_taken(item, cJSON_IsString(item))) {
        (void) cJSON_GetStringValue(item);
    }
}

void take_number(cJSON *root, const char *key)
{
    cJSON *item = config_track_get(root, key);
    if (item && config_track_taken(item, cJSON_IsNumber(item))) {
        (void) item->valueint;
    }
}

void take_bool(cJSON *root, const char *key)
{
    cJSON *item = config_track_get(root, key);
    if (item && config_track_taken(item, cJSON_IsBool(item))) {
        (void) cJSON_IsTrue(item);
    }
}

}  // namespace

TEST(ConfigTrack, EveryFieldTakenGivesNoReport)
{
    Parsed p(R"({"device_name":"frame","rotation_interval":300,"sleep_enabled":true})");
    ASSERT_TRUE(config_track_begin(p.root));
    take_string(p.root, "device_name");
    take_number(p.root, "rotation_interval");
    take_bool(p.root, "sleep_enabled");
    EXPECT_EQ(config_track_end(), nullptr);
}

TEST(ConfigTrack, AFieldOfTheWrongTypeIsIgnoredAndNamed)
{
    Parsed p(
        R"({"device_name":5,"rotation_interval":"soon","sleep_enabled":"maybe","timezone":"UTC0"})");
    ASSERT_TRUE(config_track_begin(p.root));
    take_string(p.root, "device_name");
    take_number(p.root, "rotation_interval");
    take_bool(p.root, "sleep_enabled");
    take_string(p.root, "timezone");
    cJSON *report = config_track_end();
    ASSERT_NE(report, nullptr);
    EXPECT_EQ(list(report, "ignored"), (V{"device_name", "rotation_interval", "sleep_enabled"}));
    EXPECT_EQ(cJSON_GetObjectItem(report, "unknown"), nullptr);
    cJSON_Delete(report);
}

TEST(ConfigTrack, AKeyNoHandlerLooksAtIsUnknown)
{
    Parsed p(R"({"device_name":"frame","device_nmae":"typo","status_flag":true})");
    ASSERT_TRUE(config_track_begin(p.root));
    take_string(p.root, "device_name");
    cJSON *report = config_track_end();
    ASSERT_NE(report, nullptr);
    EXPECT_EQ(list(report, "unknown"), (V{"device_nmae", "status_flag"}));
    EXPECT_EQ(cJSON_GetObjectItem(report, "ignored"), nullptr);
    cJSON_Delete(report);
}

TEST(ConfigTrack, BothListsAtOnce)
{
    Parsed p(R"({"a":1,"b":"x","c":"unused"})");
    ASSERT_TRUE(config_track_begin(p.root));
    take_string(p.root, "a");
    take_string(p.root, "b");
    cJSON *report = config_track_end();
    ASSERT_NE(report, nullptr);
    EXPECT_EQ(list(report, "ignored"), V{"a"});
    EXPECT_EQ(list(report, "unknown"), V{"c"});
    cJSON_Delete(report);
}

TEST(ConfigTrack, ALookupIsCaseInsensitiveLikeCJson)
{
    Parsed p(R"({"Device_Name":"frame"})");
    ASSERT_TRUE(config_track_begin(p.root));
    take_string(p.root, "device_name");  // cJSON finds it - and so it is taken
    EXPECT_EQ(config_track_end(), nullptr);
}

TEST(ConfigTrack, ChecksOfAnyOtherObjectAreLeftAlone)
{
    Parsed p(R"({"agenda_cron":["*/15 * *"],"nested":{"inner":"x"}})");
    ASSERT_TRUE(config_track_begin(p.root));
    cJSON *cron = config_track_get(p.root, "agenda_cron");
    ASSERT_TRUE(config_track_taken(cron, cJSON_IsArray(cron)));
    // the elements of the array and the members of the nested object are not children of the
    // request
    cJSON *entry = cJSON_GetArrayItem(cron, 0);
    EXPECT_TRUE(config_track_taken(entry, cJSON_IsString(entry)));
    cJSON *nested = config_track_get(p.root, "nested");
    cJSON *inner = config_track_get(nested, "inner");
    EXPECT_TRUE(config_track_taken(inner, cJSON_IsString(inner)));
    EXPECT_TRUE(config_track_taken(nested, cJSON_IsObject(nested)));
    EXPECT_EQ(config_track_end(), nullptr);
}

TEST(ConfigTrack, AResultPassesThroughWhateverHappens)
{
    Parsed p(R"({"x":"s"})");
    ASSERT_TRUE(config_track_begin(p.root));
    cJSON *x = config_track_get(p.root, "x");
    EXPECT_TRUE(config_track_taken(x, true));
    EXPECT_FALSE(config_track_taken(x, false));
    EXPECT_FALSE(config_track_taken(nullptr, false));
    EXPECT_TRUE(
        config_track_taken(nullptr, true));  // no item: nothing to mark, the result still passes
    cJSON_Delete(config_track_end());
}

TEST(ConfigTrack, ABoolIsTakenByIsTrueOnlyWhenItIsABool)
{
    // utils.c counts cJSON_IsTrue(item) as taken for a boolean only (see the macro there)
    Parsed p(R"({"on":true,"off":false,"text":"yes"})");
    ASSERT_TRUE(config_track_begin(p.root));
    for (const char *key : {"on", "off", "text"}) {
        cJSON *item = config_track_get(p.root, key);
        config_track_taken(item, cJSON_IsBool(item));
        (void) cJSON_IsTrue(item);
    }
    cJSON *report = config_track_end();
    ASSERT_NE(report, nullptr);
    EXPECT_EQ(list(report, "ignored"), V{"text"});
    cJSON_Delete(report);
}

TEST(ConfigTrack, JsonNullIsNoValueAndNeverListed)
{
    Parsed p(R"({"http_password":null,"timezone":null,"x":"y"})");
    ASSERT_TRUE(config_track_begin(p.root));
    take_string(p.root, "http_password");  // looked up, null is not a string - and not a mistake
    cJSON *report = config_track_end();
    ASSERT_NE(report, nullptr);
    EXPECT_EQ(cJSON_GetObjectItem(report, "ignored"), nullptr);
    EXPECT_EQ(list(report, "unknown"), V{"x"});  // "timezone": null is not listed as unknown either
    cJSON_Delete(report);
}

TEST(ConfigTrack, OnlyOneRequestIsTrackedAtATime)
{
    Parsed a(R"({"x":1})");
    Parsed b(R"({"y":"z"})");
    ASSERT_TRUE(config_track_begin(a.root));
    EXPECT_FALSE(config_track_begin(b.root));  // a second request: nothing tracked for it
    // its lookups do not disturb the first one
    cJSON *y = config_track_get(b.root, "y");
    EXPECT_TRUE(config_track_taken(y, cJSON_IsString(y)));
    take_string(a.root, "x");  // wrong type for the first request
    cJSON *report = config_track_end();
    ASSERT_NE(report, nullptr);
    EXPECT_EQ(list(report, "ignored"), V{"x"});
    cJSON_Delete(report);
    // and when it is over, the next one can start
    ASSERT_TRUE(config_track_begin(b.root));
    take_string(b.root, "y");
    EXPECT_EQ(config_track_end(), nullptr);
}

TEST(ConfigTrack, NoObjectNoTracking)
{
    Parsed list_root("[1,2]");
    EXPECT_FALSE(config_track_begin(list_root.root));
    EXPECT_FALSE(config_track_begin(nullptr));
    EXPECT_EQ(config_track_end(), nullptr);  // nothing was started
}

TEST(ConfigTrack, ARequestWithMoreKeysThanTheLimitStillWorks)
{
    cJSON *root = cJSON_CreateObject();
    for (int i = 0; i < CONFIG_TRACK_MAX_KEYS + 40; i++) {
        std::string key = "k" + std::to_string(i);
        cJSON_AddStringToObject(root, key.c_str(), "v");
    }
    ASSERT_TRUE(config_track_begin(root));
    for (int i = 0; i < CONFIG_TRACK_MAX_KEYS + 40; i += 2) {
        take_string(root, ("k" + std::to_string(i)).c_str());
    }
    cJSON *report = config_track_end();
    ASSERT_NE(report, nullptr);
    // the odd keys within the limit are unknown; those past the limit are not reported at all
    EXPECT_EQ((int) list(report, "unknown").size(), CONFIG_TRACK_MAX_KEYS / 2);
    cJSON_Delete(report);
    cJSON_Delete(root);
}

TEST(ConfigTrack, ManyRequestsInARowLeakNothingAndStayIndependent)
{
    for (int round = 0; round < 500; round++) {
        Parsed p(R"({"a":"x","b":1,"c":true})");
        ASSERT_TRUE(config_track_begin(p.root));
        take_string(p.root, "a");
        if (round % 2) {
            take_string(p.root, "b");  // wrong type on odd rounds
        } else {
            take_number(p.root, "b");
        }
        cJSON *report = config_track_end();
        if (round % 2) {
            ASSERT_NE(report, nullptr);
            EXPECT_EQ(list(report, "ignored"), V{"b"});
            EXPECT_EQ(list(report, "unknown"), V{"c"});
        } else {
            ASSERT_NE(report, nullptr);
            EXPECT_EQ(list(report, "unknown"), V{"c"});
            EXPECT_EQ(cJSON_GetObjectItem(report, "ignored"), nullptr);
        }
        cJSON_Delete(report);
    }
}
