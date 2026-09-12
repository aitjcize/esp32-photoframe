#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Modest on-device chime WAVs: mono or stereo PCM, 8/16-bit, 8–22.05 kHz,
// a few seconds. Huge or compressed files are rejected by the caller.
#define WAV_PCM_MIN_RATE 8000u
#define WAV_PCM_MAX_RATE 22050u
#define WAV_PCM_MAX_SECONDS 6u
#define WAV_PCM_MAX_FILE_BYTES (256u * 1024u)

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t data_offset;
    uint32_t data_bytes;
} wav_pcm_info_t;

// Parse a RIFF/WAVE PCM header from a memory buffer that includes the fmt
// chunk and the start of the data chunk. Returns 0 on success.
int wav_pcm_parse(const uint8_t *buf, size_t len, wav_pcm_info_t *out);

// Parse from an open file (scans chunks). On success the file position is at
// the start of PCM data. Returns 0 on success.
int wav_pcm_parse_file(FILE *f, wav_pcm_info_t *out);

bool wav_pcm_is_supported(const wav_pcm_info_t *info);

// Bytes of source PCM that may be played (duration cap, not file-size cap).
uint32_t wav_pcm_max_play_bytes(const wav_pcm_info_t *info);

// Expand source PCM frames to interleaved signed-16 stereo.
// *src_consumed (optional) is set to the number of complete source bytes used.
// Returns the number of stereo frames written to dst.
size_t wav_pcm_expand_s16_stereo(const wav_pcm_info_t *info, const uint8_t *src, size_t src_bytes,
                                 int16_t *dst, size_t max_frames, size_t *src_consumed);

#ifdef __cplusplus
}
#endif
