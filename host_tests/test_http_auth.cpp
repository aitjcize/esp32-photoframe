#include <gtest/gtest.h>

extern "C" {
#include "http_auth.h"
}

#include <array>
#include <string>

// base64("user:secret") etc. -- spelled out so the tests read as wire data.
static std::string basic(const std::string &user, const std::string &pass)
{
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string in = user + ":" + pass, out;
    for (size_t i = 0; i < in.size(); i += 3) {
        unsigned v = (unsigned char) in[i] << 16;
        size_t n = 1;
        if (i + 1 < in.size()) {
            v |= (unsigned char) in[i + 1] << 8;
            n++;
        }
        if (i + 2 < in.size()) {
            v |= (unsigned char) in[i + 2];
            n++;
        }
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += (n > 1) ? tbl[(v >> 6) & 63] : '=';
        out += (n > 2) ? tbl[v & 63] : '=';
    }
    return "Basic " + out;
}

// An unset password means the feature is off -- every request must pass, with
// or without an Authorization header. This is the default state.
TEST(HttpAuth, DisabledWhenNoPasswordConfigured)
{
    EXPECT_TRUE(http_auth_header_matches(nullptr, ""));
    EXPECT_TRUE(http_auth_header_matches(nullptr, nullptr));
    EXPECT_TRUE(http_auth_header_matches(basic("u", "anything").c_str(), ""));
}

TEST(HttpAuth, AcceptsCorrectPassword)
{
    EXPECT_TRUE(http_auth_header_matches(basic("admin", "hunter2").c_str(), "hunter2"));
}

// The username half is not part of the secret.
TEST(HttpAuth, IgnoresUsername)
{
    EXPECT_TRUE(http_auth_header_matches(basic("", "hunter2").c_str(), "hunter2"));
    EXPECT_TRUE(http_auth_header_matches(basic("somebody-else", "hunter2").c_str(), "hunter2"));
}

TEST(HttpAuth, RejectsWrongPassword)
{
    EXPECT_FALSE(http_auth_header_matches(basic("admin", "hunter3").c_str(), "hunter2"));
    EXPECT_FALSE(http_auth_header_matches(basic("admin", "").c_str(), "hunter2"));
}

// A correct prefix must not be accepted -- the comparison is length-aware.
TEST(HttpAuth, RejectsPrefixOfThePassword)
{
    EXPECT_FALSE(http_auth_header_matches(basic("admin", "hunter").c_str(), "hunter2"));
    EXPECT_FALSE(http_auth_header_matches(basic("admin", "hunter22").c_str(), "hunter2"));
}

TEST(HttpAuth, RequiresBasicScheme)
{
    EXPECT_FALSE(http_auth_header_matches("Bearer hunter2", "hunter2"));
    EXPECT_FALSE(http_auth_header_matches("hunter2", "hunter2"));
    EXPECT_FALSE(http_auth_header_matches(nullptr, "hunter2"));
    EXPECT_FALSE(http_auth_header_matches("", "hunter2"));
}

// Scheme token is case-insensitive per RFC 7235.
TEST(HttpAuth, SchemeIsCaseInsensitive)
{
    std::string h = basic("admin", "hunter2");
    h[0] = 'b';
    EXPECT_TRUE(http_auth_header_matches(h.c_str(), "hunter2"));
}

// A password may contain ':' -- only the first separator splits the credential.
TEST(HttpAuth, PasswordMayContainColon)
{
    EXPECT_TRUE(http_auth_header_matches(basic("admin", "a:b:c").c_str(), "a:b:c"));
}

TEST(HttpAuth, RejectsMalformedBase64)
{
    EXPECT_FALSE(http_auth_header_matches("Basic !!!!not-base64!!!!", "hunter2"));
    EXPECT_FALSE(http_auth_header_matches("Basic ", "hunter2"));
}

// No ':' at all is not a credential, even if the text equals the password.
TEST(HttpAuth, RejectsCredentialWithoutSeparator)
{
    EXPECT_FALSE(http_auth_header_matches("Basic aHVudGVyMg==", "hunter2"));  // "hunter2"
}

// An over-long credential must be refused rather than truncated into a match.
TEST(HttpAuth, RejectsOverlongCredential)
{
    EXPECT_FALSE(
        http_auth_header_matches(basic(std::string(400, 'u'), "hunter2").c_str(), "hunter2"));
}

// ---------------------------------------------------------------------------
// Brute-force limiter

class HttpAuthLimiter : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        http_auth_limiter_reset();
    }

    static std::array<uint8_t, HTTP_AUTH_ADDR_LEN> ip(uint8_t last)
    {
        std::array<uint8_t, HTTP_AUTH_ADDR_LEN> a{};
        a[10] = 0xff;
        a[11] = 0xff;
        a[12] = 192;
        a[13] = 168;
        a[15] = last;
        return a;
    }

    static void fail(const std::array<uint8_t, HTTP_AUTH_ADDR_LEN> &a, int n, int64_t now)
    {
        for (int i = 0; i < n; i++) {
            http_auth_limiter_record(a.data(), false, now);
        }
    }
};

// A typo or two must never lock the owner out.
TEST_F(HttpAuthLimiter, FreeFailuresDoNotLockOut)
{
    auto a = ip(10);
    fail(a, HTTP_AUTH_FREE_FAILURES, 1000);
    EXPECT_TRUE(http_auth_limiter_allowed(a.data(), 1000, nullptr));
}

TEST_F(HttpAuthLimiter, LocksOutAfterFreeFailuresThenExpires)
{
    auto a = ip(10);
    fail(a, HTTP_AUTH_FREE_FAILURES + 1, 1000);

    int64_t retry = 0;
    EXPECT_FALSE(http_auth_limiter_allowed(a.data(), 1000, &retry));
    EXPECT_EQ(retry, HTTP_AUTH_LOCKOUT_BASE_MS);
    EXPECT_FALSE(
        http_auth_limiter_allowed(a.data(), 1000 + HTTP_AUTH_LOCKOUT_BASE_MS - 1, nullptr));
    EXPECT_TRUE(http_auth_limiter_allowed(a.data(), 1000 + HTTP_AUTH_LOCKOUT_BASE_MS, nullptr));
}

TEST_F(HttpAuthLimiter, LockoutDoublesAndIsCapped)
{
    auto a = ip(10);
    fail(a, HTTP_AUTH_FREE_FAILURES + 2, 0);  // second failure past the free ones
    int64_t retry = 0;
    EXPECT_FALSE(http_auth_limiter_allowed(a.data(), 0, &retry));
    EXPECT_EQ(retry, 2 * HTTP_AUTH_LOCKOUT_BASE_MS);

    fail(a, 100, 0);
    EXPECT_FALSE(http_auth_limiter_allowed(a.data(), 0, &retry));
    EXPECT_EQ(retry, HTTP_AUTH_LOCKOUT_MAX_MS);
}

TEST_F(HttpAuthLimiter, SuccessClearsTheRecord)
{
    auto a = ip(10);
    fail(a, HTTP_AUTH_FREE_FAILURES, 0);
    http_auth_limiter_record(a.data(), true, 0);
    fail(a, HTTP_AUTH_FREE_FAILURES, 0);  // a fresh set of free failures
    EXPECT_TRUE(http_auth_limiter_allowed(a.data(), 0, nullptr));
}

// One client guessing must not lock out another.
TEST_F(HttpAuthLimiter, ClientsAreTrackedSeparately)
{
    auto attacker = ip(66), owner = ip(10);
    fail(attacker, HTTP_AUTH_FREE_FAILURES + 1, 0);
    EXPECT_FALSE(http_auth_limiter_allowed(attacker.data(), 0, nullptr));
    EXPECT_TRUE(http_auth_limiter_allowed(owner.data(), 0, nullptr));
}

// A full table forgets the client seen least recently -- not the one that
// arrived first -- and never refuses unknown clients. `refreshed` is the
// oldest entry but guesses again after every newcomer, so it is never the
// least recently seen; `older` is quiet after its lockout and goes first.
// Written without knowing the table size: newcomers keep arriving until it
// has overflowed many times over.
TEST_F(HttpAuthLimiter, FullTableEvictsLeastRecentlySeen)
{
    auto refreshed = ip(1), older = ip(2);
    fail(refreshed, HTTP_AUTH_FREE_FAILURES + 1, 0);
    fail(older, HTTP_AUTH_FREE_FAILURES + 1, 10);
    for (uint8_t i = 3; i < 40; i++) {
        auto newcomer = ip(i);
        http_auth_limiter_record(newcomer.data(), false, 100 + i);
        http_auth_limiter_record(refreshed.data(), false, 100 + i);
    }
    // Evicted: its lockout would otherwise still be running
    EXPECT_TRUE(http_auth_limiter_allowed(older.data(), 200, nullptr));
    // Kept, and still locked out
    EXPECT_FALSE(http_auth_limiter_allowed(refreshed.data(), 200, nullptr));
    auto fresh = ip(200);
    EXPECT_TRUE(http_auth_limiter_allowed(fresh.data(), 200, nullptr));
}
