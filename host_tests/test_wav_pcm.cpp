#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "wav_pcm.h"
}

namespace
{

void put_le16(std::vector<uint8_t> &b, uint16_t v)
{
    b.push_back((uint8_t) (v & 0xff));
    b.push_back((uint8_t) (v >> 8));
}

void put_le32(std::vector<uint8_t> &b, uint32_t v)
{
    b.push_back((uint8_t) (v & 0xff));
    b.push_back((uint8_t) ((v >> 8) & 0xff));
    b.push_back((uint8_t) ((v >> 16) & 0xff));
    b.push_back((uint8_t) (v >> 24));
}

std::vector<uint8_t> make_wav(uint16_t channels, uint32_t rate, uint16_t bits,
                              const std::vector<uint8_t> &pcm, bool extra_list_chunk = false)
{
    std::vector<uint8_t> fmt;
    put_le16(fmt, 1);  // PCM
    put_le16(fmt, channels);
    put_le32(fmt, rate);
    uint32_t byte_rate = rate * channels * (bits / 8);
    put_le32(fmt, byte_rate);
    put_le16(fmt, (uint16_t) (channels * (bits / 8)));
    put_le16(fmt, bits);

    std::vector<uint8_t> extra;
    if (extra_list_chunk) {
        extra.insert(extra.end(), {'L', 'I', 'S', 'T'});
        put_le32(extra, 4);
        extra.insert(extra.end(), {'I', 'N', 'F', 'O'});
    }

    uint32_t riff_size =
        4 + 8 + (uint32_t) fmt.size() + (uint32_t) extra.size() + 8 + (uint32_t) pcm.size();
    std::vector<uint8_t> wav;
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    put_le32(wav, riff_size);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});
    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    put_le32(wav, (uint32_t) fmt.size());
    wav.insert(wav.end(), fmt.begin(), fmt.end());
    wav.insert(wav.end(), extra.begin(), extra.end());
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    put_le32(wav, (uint32_t) pcm.size());
    wav.insert(wav.end(), pcm.begin(), pcm.end());
    return wav;
}

}  // namespace

TEST(WavPcm, ParsesMono16)
{
    std::vector<uint8_t> pcm(16, 0);
    auto wav = make_wav(1, 16000, 16, pcm);
    wav_pcm_info_t info;
    ASSERT_EQ(wav_pcm_parse(wav.data(), wav.size(), &info), 0);
    EXPECT_EQ(info.sample_rate, 16000u);
    EXPECT_EQ(info.channels, 1);
    EXPECT_EQ(info.bits_per_sample, 16);
    EXPECT_EQ(info.data_bytes, 16u);
    EXPECT_TRUE(wav_pcm_is_supported(&info));
}

TEST(WavPcm, ParsesStereo8WithListChunk)
{
    std::vector<uint8_t> pcm = {128, 128, 200, 50};
    auto wav = make_wav(2, 8000, 8, pcm, true);
    wav_pcm_info_t info;
    ASSERT_EQ(wav_pcm_parse(wav.data(), wav.size(), &info), 0);
    EXPECT_EQ(info.sample_rate, 8000u);
    EXPECT_EQ(info.channels, 2);
    EXPECT_EQ(info.bits_per_sample, 8);
    EXPECT_TRUE(wav_pcm_is_supported(&info));
}

TEST(WavPcm, RejectsCompressedAndOutOfRange)
{
    std::vector<uint8_t> pcm(8, 0);
    auto wav = make_wav(1, 16000, 16, pcm);
    wav[20] = 3;  // AudioFormat = 3 (IEEE float)
    wav_pcm_info_t info;
    EXPECT_NE(wav_pcm_parse(wav.data(), wav.size(), &info), 0);

    auto hi = make_wav(1, 44100, 16, pcm);
    ASSERT_EQ(wav_pcm_parse(hi.data(), hi.size(), &info), 0);
    EXPECT_FALSE(wav_pcm_is_supported(&info));

    auto bad_magic = wav;
    bad_magic[0] = 'X';
    EXPECT_NE(wav_pcm_parse(bad_magic.data(), bad_magic.size(), &info), 0);
}

TEST(WavPcm, CapsPlayDuration)
{
    // 16-bit mono @ 8 kHz: 8 seconds of silence is 128000 bytes of PCM.
    std::vector<uint8_t> pcm(8000 * 2 * 8, 0);
    auto wav = make_wav(1, 8000, 16, pcm);
    wav_pcm_info_t info;
    ASSERT_EQ(wav_pcm_parse(wav.data(), wav.size(), &info), 0);
    EXPECT_EQ(wav_pcm_max_play_bytes(&info), 8000u * 2u * WAV_PCM_MAX_SECONDS);
}

TEST(WavPcm, Expands8BitMonoAnd16BitStereo)
{
    wav_pcm_info_t mono8 = {};
    mono8.sample_rate = 8000;
    mono8.channels = 1;
    mono8.bits_per_sample = 8;
    mono8.data_bytes = 2;
    uint8_t src8[] = {128, 255};
    int16_t dst[8] = {0};
    size_t consumed = 0;
    EXPECT_EQ(wav_pcm_expand_s16_stereo(&mono8, src8, sizeof(src8), dst, 4, &consumed), 2u);
    EXPECT_EQ(consumed, 2u);
    EXPECT_EQ(dst[0], 0);
    EXPECT_EQ(dst[1], 0);
    EXPECT_EQ(dst[2], (int16_t) (((int) 255 - 128) << 8));
    EXPECT_EQ(dst[3], dst[2]);

    wav_pcm_info_t st16 = {};
    st16.sample_rate = 16000;
    st16.channels = 2;
    st16.bits_per_sample = 16;
    st16.data_bytes = 4;
    uint8_t src16[] = {0x00, 0x10, 0xff, 0x7f};  // 0x1000, 0x7fff
    memset(dst, 0, sizeof(dst));
    EXPECT_EQ(wav_pcm_expand_s16_stereo(&st16, src16, sizeof(src16), dst, 2, &consumed), 1u);
    EXPECT_EQ(dst[0], 0x1000);
    EXPECT_EQ(dst[1], 0x7fff);
}

TEST(WavPcm, ParseFileRoundTrip)
{
    std::vector<uint8_t> pcm = {0x11, 0x22, 0x33, 0x44};
    auto wav = make_wav(1, 22050, 16, pcm, true);
    char path[] = "/tmp/chime_wav_pcm_XXXXXX";
    int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    FILE *out = fdopen(fd, "wb");
    ASSERT_TRUE(out);
    ASSERT_EQ(fwrite(wav.data(), 1, wav.size(), out), wav.size());
    fclose(out);

    FILE *in = fopen(path, "rb");
    ASSERT_TRUE(in);
    wav_pcm_info_t info;
    ASSERT_EQ(wav_pcm_parse_file(in, &info), 0);
    EXPECT_EQ(info.sample_rate, 22050u);
    EXPECT_EQ(info.channels, 1);
    EXPECT_EQ(info.bits_per_sample, 16);
    EXPECT_EQ(info.data_bytes, 4u);
    uint8_t got[4];
    ASSERT_EQ(fread(got, 1, 4, in), 4u);
    EXPECT_EQ(memcmp(got, pcm.data(), 4), 0);
    fclose(in);
    unlink(path);
}
