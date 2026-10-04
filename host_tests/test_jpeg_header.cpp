// The frame size tjpgd will decode: the last SOF0 before SOS, with the
// non-baseline SOF markers it refuses rejected and a short file reported as
// malformed.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

extern "C" {
#include "jpeg_header.h"
}

namespace
{

using Bytes = std::vector<uint8_t>;

void append(Bytes &out, const Bytes &more)
{
    out.insert(out.end(), more.begin(), more.end());
}

// A marker segment; the 16-bit length counts its own two bytes
Bytes segment(uint8_t marker, const Bytes &payload)
{
    size_t len = payload.size() + 2;
    Bytes out = {0xFF, marker, uint8_t(len >> 8), uint8_t(len & 0xFF)};
    append(out, payload);
    return out;
}

// SOFn: 8-bit precision, height, width, one grey component
Bytes frame(uint8_t marker, int width, int height)
{
    return segment(marker, {8, uint8_t(height >> 8), uint8_t(height & 0xFF), uint8_t(width >> 8),
                            uint8_t(width & 0xFF), 1, 1, 0x11, 0});
}

const Bytes kSoi = {0xFF, 0xD8};
const Bytes kSos = segment(0xDA, {1, 1, 0x00, 0, 63, 0});

bool size_of(const Bytes &file, int *w, int *h)
{
    return jpeg_header_frame_size(file.data(), file.size(), w, h);
}

}  // namespace

TEST(JpegHeader, SingleSof0ReportsItsSize)
{
    Bytes file = kSoi;
    append(file, frame(0xC0, 160, 152));
    append(file, kSos);

    int w = 0, h = 0;
    ASSERT_TRUE(size_of(file, &w, &h));
    EXPECT_EQ(w, 160);
    EXPECT_EQ(h, 152);
}

TEST(JpegHeader, LastSof0BeforeSosWins)
{
    Bytes file = kSoi;
    append(file, frame(0xC0, 160, 152));
    append(file, frame(0xC0, 40000, 35792));
    append(file, kSos);

    int w = 0, h = 0;
    ASSERT_TRUE(size_of(file, &w, &h));
    EXPECT_EQ(w, 40000);
    EXPECT_EQ(h, 35792);
}

TEST(JpegHeader, App0AndCommentSegmentsAreSkipped)
{
    Bytes file = kSoi;
    append(file, segment(0xE0, {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0}));
    append(file, segment(0xFE, {'h', 'i'}));
    append(file, frame(0xC0, 160, 152));
    append(file, segment(0xDB, Bytes(65, 0)));
    append(file, kSos);

    int w = 0, h = 0;
    ASSERT_TRUE(size_of(file, &w, &h));
    EXPECT_EQ(w, 160);
    EXPECT_EQ(h, 152);
}

TEST(JpegHeader, OneFillByteBeforeAMarkerIsSkippedLikeTjpgd)
{
    Bytes file = kSoi;
    file.push_back(0xFF);
    append(file, frame(0xC0, 160, 152));
    append(file, kSos);

    int w = 0, h = 0;
    ASSERT_TRUE(size_of(file, &w, &h));
    EXPECT_EQ(w, 160);
    EXPECT_EQ(h, 152);
}

TEST(JpegHeader, ProgressiveSof2IsRejected)
{
    Bytes file = kSoi;
    append(file, frame(0xC2, 160, 152));
    append(file, kSos);

    int w = 0, h = 0;
    EXPECT_FALSE(size_of(file, &w, &h));
}

TEST(JpegHeader, TruncatedSegmentIsRejected)
{
    Bytes file = kSoi;
    append(file, frame(0xC0, 160, 152));
    append(file, kSos);
    file.resize(file.size() - 2);

    int w = 0, h = 0;
    EXPECT_FALSE(size_of(file, &w, &h));

    Bytes no_sos = kSoi;
    append(no_sos, frame(0xC0, 160, 152));
    EXPECT_FALSE(size_of(no_sos, &w, &h));
}

TEST(JpegHeader, MissingSoiOrFrameIsRejected)
{
    Bytes not_jpeg = {0x89, 'P', 'N', 'G'};
    int w = 0, h = 0;
    EXPECT_FALSE(size_of(not_jpeg, &w, &h));

    Bytes no_frame = kSoi;
    append(no_frame, kSos);
    EXPECT_FALSE(size_of(no_frame, &w, &h));

    Bytes zero_size = kSoi;
    append(zero_size, frame(0xC0, 0, 152));
    append(zero_size, kSos);
    EXPECT_FALSE(size_of(zero_size, &w, &h));
}
