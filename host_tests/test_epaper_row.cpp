// The row copies the panel drivers use to mirror the picture as they send it
// (#161), and how the IT8951 driver combines them with the panel's scan
// direction.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

extern "C" {
#include "epaper_row.h"
}

namespace
{

using Row = std::vector<uint8_t>;

uint8_t pixel(const Row &row, int x)
{
    return (x % 2 == 0) ? (row[x / 2] >> 4) : (row[x / 2] & 0x0F);
}

// Every pixel gets a value from its position so a misplaced one shows up
Row pattern(int width)
{
    Row row(width / 2, 0);
    for (int x = 0; x < width; x++) {
        uint8_t v = (x * 7 + 3) & 0x0F;
        uint8_t &b = row[x / 2];
        b = (x % 2 == 0) ? ((b & 0x0F) | (v << 4)) : ((b & 0xF0) | v);
    }
    return row;
}

Row reversePixels(const Row &src)
{
    Row dst(src.size());
    epaper_row_reverse_pixels(dst.data(), src.data(), src.size());
    return dst;
}

Row reverseWords(const Row &src)
{
    Row dst(src.size());
    epaper_row_reverse_words(dst.data(), src.data(), src.size());
    return dst;
}

Row reversePixelsInWords(const Row &src)
{
    Row dst(src.size());
    epaper_row_reverse_pixels_in_words(dst.data(), src.data(), src.size());
    return dst;
}

// The IT8951 driver's row copy, as in driver_it8951.c
Row it8951Send(const Row &row, bool scan_right_to_left, bool mirror)
{
    if (scan_right_to_left && mirror) {
        return reversePixelsInWords(row);
    }
    if (scan_right_to_left) {
        return reverseWords(row);
    }
    if (mirror) {
        return reversePixels(row);
    }
    return row;
}

// What the glass shows, left to right: the IT8951 unpacks each 16-bit word's
// 4 pixels in frame-buffer order when loading big-endian and reversed when
// little-endian (the driver loads little-endian exactly for right-to-left
// panels), and a right-to-left panel puts the first pixel at the right edge.
std::vector<uint8_t> glass(const Row &sent, bool scan_right_to_left)
{
    const int width = (int) sent.size() * 2;
    std::vector<uint8_t> scan;
    for (int w = 0; w < width / 4; w++) {
        for (int i = 0; i < 4; i++) {
            scan.push_back(pixel(sent, w * 4 + (scan_right_to_left ? 3 - i : i)));
        }
    }
    std::vector<uint8_t> out(width);
    for (int k = 0; k < width; k++) {
        out[scan_right_to_left ? width - 1 - k : k] = scan[k];
    }
    return out;
}

}  // namespace

TEST(EpaperRow, ReversePixelsMirrorsTheRow)
{
    for (int width : {8, 12, 800, 1200, 1872}) {
        Row row = pattern(width);
        Row out = reversePixels(row);
        for (int x = 0; x < width; x++) {
            ASSERT_EQ(pixel(out, x), pixel(row, width - 1 - x)) << "width " << width << " x " << x;
        }
        EXPECT_EQ(reversePixels(out), row);
    }
}

TEST(EpaperRow, ReversePixelsInPiecesBuildsTheMirroredRow)
{
    // The 7.3" driver sends 400-byte rows in 128-byte chunks; the 13.3" one
    // sends 600-byte rows as two 300-byte halves, one per controller.
    for (auto [row_bytes, piece] : {std::pair<size_t, size_t>{400, 128}, {600, 300}}) {
        Row row = pattern((int) row_bytes * 2);
        Row whole = reversePixels(row);
        Row pieces;
        for (size_t off = 0; off < row_bytes; off += piece) {
            size_t n = std::min(piece, row_bytes - off);
            Row part(n);
            epaper_row_reverse_pixels(part.data(), row.data() + row_bytes - off - n, n);
            pieces.insert(pieces.end(), part.begin(), part.end());
        }
        EXPECT_EQ(pieces, whole) << "row " << row_bytes << " piece " << piece;
    }
}

TEST(EpaperRow, ReverseWordsMatchesTheOldIt8951Loop)
{
    Row row = pattern(1872);
    const size_t row_words = row.size() / 2;
    Row old(row.size());
    for (size_t wi = 0; wi < row_words; wi++) {
        old[wi * 2] = row[(row_words - 1 - wi) * 2];
        old[wi * 2 + 1] = row[(row_words - 1 - wi) * 2 + 1];
    }
    EXPECT_EQ(reverseWords(row), old);
}

TEST(EpaperRow, PixelsInWordsIsWordsAfterPixels)
{
    // On a right-to-left panel the driver now sends exactly what the frame
    // buffer flip plus word reversal sent before, the mirror the #161
    // reporter confirmed on an ES103TC1.
    Row row = pattern(1872);
    EXPECT_EQ(reversePixelsInWords(row), reverseWords(reversePixels(row)));
}

TEST(EpaperRow, It8951ShowsThePictureForEveryCombination)
{
    Row row = pattern(1872);
    std::vector<uint8_t> straight(1872), mirrored(1872);
    for (int x = 0; x < 1872; x++) {
        straight[x] = pixel(row, x);
        mirrored[x] = pixel(row, 1871 - x);
    }
    for (bool rtl : {false, true}) {
        EXPECT_EQ(glass(it8951Send(row, rtl, false), rtl), straight) << "rtl " << rtl;
        EXPECT_EQ(glass(it8951Send(row, rtl, true), rtl), mirrored) << "rtl " << rtl;
    }
}
