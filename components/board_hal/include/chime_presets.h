#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Built-in ES8311 sine presets. Tables are public-domain note sequences only
// (Mozart K.525 opening, Beethoven 9 / Ode to Joy, Frère Jacques, Twinkle,
// plus short flourishes). No copyrighted game or OS ringtones.
#define CHIME_PRESET_DEFAULT "mozart"
#define CHIME_PRESET_FALLBACK "triad"
#define CHIME_PRESET_MAX_MS 8000

// A4=440 equal temperament. 0 Hz is a rest.
#define NOTE_REST 0.0f
#define NOTE_C4 261.63f
#define NOTE_D4 293.66f
#define NOTE_E4 329.63f
#define NOTE_F4 349.23f
#define NOTE_FS4 369.99f
#define NOTE_G4 392.00f
#define NOTE_A4 440.00f
#define NOTE_B4 493.88f
#define NOTE_C5 523.25f
#define NOTE_D5 587.33f
#define NOTE_E5 659.26f
#define NOTE_F5 698.46f
#define NOTE_G5 783.99f
#define NOTE_A5 880.00f
#define NOTE_C6 1046.50f

typedef struct {
    float freq_hz;
    uint16_t duration_ms;
} chime_note_t;

bool chime_preset_id_is_valid(const char *preset);

// Empty/NULL resolves to CHIME_PRESET_DEFAULT. `ascending` shares the fanfare
// table. Returns false if the name is unknown.
bool chime_preset_lookup(const char *preset, const chime_note_t **notes, size_t *count);

// Sum of table durations in milliseconds (not playback-capped). -1 if unknown.
int chime_preset_duration_ms(const char *preset);

const char *const *chime_preset_ids(size_t *count);

#ifdef __cplusplus
}
#endif
