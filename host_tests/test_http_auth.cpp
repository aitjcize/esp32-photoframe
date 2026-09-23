#include <gtest/gtest.h>

extern "C" {
#include "http_auth.h"
}

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
