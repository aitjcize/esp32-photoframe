#include "wav_pcm.h"

#include <stdio.h>
#include <string.h>

void wav_pcm_max_file_label(char *dest, size_t dest_len)
{
    if (!dest || dest_len == 0) {
        return;
    }
    unsigned mib = WAV_PCM_MAX_FILE_BYTES / (1024u * 1024u);
    if (mib > 0 && (WAV_PCM_MAX_FILE_BYTES % (1024u * 1024u)) == 0) {
        snprintf(dest, dest_len, "%u MiB", mib);
        return;
    }
    snprintf(dest, dest_len, "%u KiB", WAV_PCM_MAX_FILE_BYTES / 1024u);
}

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}

static int parse_fmt_chunk(const uint8_t *fmt, size_t size, wav_pcm_info_t *out)
{
    if (size < 16) {
        return -1;
    }
    // AudioFormat 1 = PCM. Reject IEEE float / extensible / compressed.
    if (rd_le16(fmt) != 1) {
        return -1;
    }
    out->channels = rd_le16(fmt + 2);
    out->sample_rate = rd_le32(fmt + 4);
    out->bits_per_sample = rd_le16(fmt + 14);
    return 0;
}

int wav_pcm_parse(const uint8_t *buf, size_t len, wav_pcm_info_t *out)
{
    if (!buf || !out || len < 12) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return -1;
    }

    bool have_fmt = false;
    size_t offset = 12;
    while (offset + 8 <= len) {
        const uint8_t *hdr = buf + offset;
        uint32_t size = rd_le32(hdr + 4);
        size_t payload = offset + 8;
        if (memcmp(hdr, "fmt ", 4) == 0) {
            if (payload + 16 > len) {
                return -1;
            }
            if (parse_fmt_chunk(buf + payload, size, out) != 0) {
                return -1;
            }
            have_fmt = true;
        } else if (memcmp(hdr, "data", 4) == 0) {
            if (!have_fmt) {
                return -1;
            }
            out->data_offset = (uint32_t) payload;
            out->data_bytes = size;
            return 0;
        }
        uint64_t next = (uint64_t) payload + size + (size & 1u);
        if (next > len) {
            return -1;
        }
        offset = (size_t) next;
    }
    return -1;
}

int wav_pcm_parse_file(FILE *f, wav_pcm_info_t *out)
{
    if (!f || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    uint8_t riff[12];
    if (fread(riff, 1, 12, f) != 12) {
        return -1;
    }
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        return -1;
    }

    bool have_fmt = false;
    for (;;) {
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, f) != 8) {
            return -1;
        }
        uint32_t size = rd_le32(hdr + 4);
        if (memcmp(hdr, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (size < 16 || fread(fmt, 1, 16, f) != 16) {
                return -1;
            }
            if (parse_fmt_chunk(fmt, size, out) != 0) {
                return -1;
            }
            long extra = (long) (size - 16) + (long) (size & 1u);
            if (extra > 0 && fseek(f, extra, SEEK_CUR) != 0) {
                return -1;
            }
            have_fmt = true;
        } else if (memcmp(hdr, "data", 4) == 0) {
            long pos = ftell(f);
            if (pos < 0 || !have_fmt) {
                return -1;
            }
            out->data_offset = (uint32_t) pos;
            out->data_bytes = size;
            if (fseek(f, 0, SEEK_END) != 0) {
                return -1;
            }
            long file_end = ftell(f);
            if (file_end < 0 || !wav_pcm_data_is_complete(out, (size_t) file_end)) {
                return -1;
            }
            if (fseek(f, pos, SEEK_SET) != 0) {
                return -1;
            }
            return 0;
        } else {
            long skip = (long) size + (long) (size & 1u);
            if (fseek(f, skip, SEEK_CUR) != 0) {
                return -1;
            }
        }
    }
}

bool wav_pcm_data_is_complete(const wav_pcm_info_t *info, size_t file_bytes)
{
    if (!info) {
        return false;
    }
    uint64_t need = (uint64_t) info->data_offset + (uint64_t) info->data_bytes;
    return (uint64_t) file_bytes >= need;
}

bool wav_pcm_is_supported(const wav_pcm_info_t *info)
{
    if (!info) {
        return false;
    }
    if (info->channels < 1 || info->channels > 2) {
        return false;
    }
    if (info->bits_per_sample != 8 && info->bits_per_sample != 16) {
        return false;
    }
    if (info->sample_rate < WAV_PCM_MIN_RATE || info->sample_rate > WAV_PCM_MAX_RATE) {
        return false;
    }
    if (info->data_bytes == 0) {
        return false;
    }
    return true;
}

uint32_t wav_pcm_max_play_bytes(const wav_pcm_info_t *info)
{
    if (!info || info->channels == 0 || info->bits_per_sample == 0 || info->sample_rate == 0) {
        return 0;
    }
    uint32_t bytes_per_frame = (uint32_t) info->channels * (info->bits_per_sample / 8u);
    if (bytes_per_frame == 0) {
        return 0;
    }
    uint64_t cap = (uint64_t) info->sample_rate * bytes_per_frame * WAV_PCM_MAX_SECONDS;
    if (cap > info->data_bytes) {
        cap = info->data_bytes;
    }
    return (uint32_t) (cap - (cap % bytes_per_frame));
}

size_t wav_pcm_expand_s16_stereo(const wav_pcm_info_t *info, const uint8_t *src, size_t src_bytes,
                                 int16_t *dst, size_t max_frames, size_t *src_consumed)
{
    if (src_consumed) {
        *src_consumed = 0;
    }
    if (!info || !src || !dst || max_frames == 0) {
        return 0;
    }

    size_t src_frame = (size_t) info->channels * (info->bits_per_sample / 8u);
    if (src_frame == 0) {
        return 0;
    }
    size_t frames = src_bytes / src_frame;
    if (frames > max_frames) {
        frames = max_frames;
    }

    for (size_t i = 0; i < frames; i++) {
        const uint8_t *frame = src + i * src_frame;
        int16_t left;
        int16_t right;
        if (info->bits_per_sample == 8) {
            left = (int16_t) (((int) frame[0] - 128) << 8);
            right = (info->channels == 2) ? (int16_t) (((int) frame[1] - 128) << 8) : left;
        } else {
            left = (int16_t) rd_le16(frame);
            right = (info->channels == 2) ? (int16_t) rd_le16(frame + 2) : left;
        }
        dst[i * 2] = left;
        dst[i * 2 + 1] = right;
    }

    if (src_consumed) {
        *src_consumed = frames * src_frame;
    }
    return frames;
}
