// Mirroring the packed 4-bit frame buffer for panels that scan the other way
// (#161): pixel x lands at width - 1 - x on every row, padding is left alone,
// and mirroring twice restores the buffer.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

extern "C" {
#include "frame_mirror.h"
}

namespace
{

int rowBytes(int width)
{
    return (width + 1) / 2;
}

uint8_t pixel(const std::vector<uint8_t> &buf, int width, int x, int y)
{
    uint8_t b = buf[y * rowBytes(width) + x / 2];
    return (x % 2 == 0) ? (b >> 4) : (b & 0x0F);
}

// Every pixel gets a value from its position so a misplaced one shows up
std::vector<uint8_t> pattern(int width, int height)
{
    std::vector<uint8_t> buf(rowBytes(width) * height, 0);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t v = (x * 7 + y * 3) & 0x0F;
            uint8_t &b = buf[y * rowBytes(width) + x / 2];
            b = (x % 2 == 0) ? ((b & 0x0F) | (v << 4)) : ((b & 0xF0) | v);
        }
    }
    return buf;
}

void expectMirrored(int width, int height)
{
    std::vector<uint8_t> orig = pattern(width, height);
    std::vector<uint8_t> buf = orig;
    frame_mirror_x(buf.data(), width, height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            EXPECT_EQ(pixel(buf, width, x, y), pixel(orig, width, width - 1 - x, y))
                << "width " << width << " at (" << x << ", " << y << ")";
        }
    }
    frame_mirror_x(buf.data(), width, height);
    EXPECT_EQ(buf, orig) << "width " << width;
}

}  // namespace

TEST(FrameMirror, EvenWidthWithEvenRowBytes)
{
    expectMirrored(8, 3);
}

TEST(FrameMirror, EvenWidthWithOddRowBytes)
{
    expectMirrored(6, 3);
}

TEST(FrameMirror, OddWidthLeavesPaddingAlone)
{
    std::vector<uint8_t> buf = pattern(5, 2);
    for (int y = 0; y < 2; y++) {
        buf[y * rowBytes(5) + 2] |= 0x0A;  // padding nibble
    }
    std::vector<uint8_t> orig = buf;
    frame_mirror_x(buf.data(), 5, 2);
    for (int y = 0; y < 2; y++) {
        EXPECT_EQ(buf[y * rowBytes(5) + 2] & 0x0F, 0x0A);
        for (int x = 0; x < 5; x++) {
            EXPECT_EQ(pixel(buf, 5, x, y), pixel(orig, 5, 4 - x, y));
        }
    }
    expectMirrored(7, 4);
}

TEST(FrameMirror, PanelSizes)
{
    expectMirrored(1872, 2);  // ED103TC2 / ES103TC1
    expectMirrored(800, 2);   // 7.3" Spectra 6
}
