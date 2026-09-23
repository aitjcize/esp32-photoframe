#include "chime_presets.h"

#include <string.h>

// Cheerful chime tempi: quarter ≈ 220–280 ms (not concert allegro).
#define MOZ_E 125
#define MOZ_Q 250
#define MOZ_DH 375
#define MOZ_R 100
#define MOZ_R2 80

#define ODE_Q 230
#define ODE_E 115
#define ODE_H 460
#define ODE_DH 345

#define FOLK_Q 250
#define FOLK_H 500

#define FAN_E 130
#define FAN_Q 260
#define FAN_H 520

#define TRI_E 110
#define TRI_H 440

static const chime_note_t k_triad[] = {
    // Major arpeggio flourish, ~2 s (C4 → C6 and back to a held C6).
    {NOTE_C4, TRI_E}, {NOTE_E4, TRI_E}, {NOTE_G4, TRI_E}, {NOTE_C5, TRI_E}, {NOTE_E5, TRI_E},
    {NOTE_G5, TRI_E}, {NOTE_C6, TRI_E}, {NOTE_G5, TRI_E}, {NOTE_E5, TRI_E}, {NOTE_C5, TRI_E},
    {NOTE_G4, TRI_E}, {NOTE_C5, TRI_E}, {NOTE_E5, TRI_E}, {NOTE_G5, TRI_E}, {NOTE_C6, TRI_H},
};

static const chime_note_t k_mozart[] = {
    // Eine kleine Nachtmusik K.525 opening (G major fanfare + answer). ~4.8 s.
    {NOTE_G4, MOZ_E}, {NOTE_D5, MOZ_E},  {NOTE_G4, MOZ_E}, {NOTE_D5, MOZ_E},  {NOTE_G4, MOZ_E},
    {NOTE_D5, MOZ_E}, {NOTE_G4, MOZ_E},  {NOTE_B4, MOZ_E}, {NOTE_D5, MOZ_Q},  {NOTE_REST, MOZ_R},
    {NOTE_C5, MOZ_E}, {NOTE_A4, MOZ_E},  {NOTE_C5, MOZ_E}, {NOTE_A4, MOZ_E},  {NOTE_C5, MOZ_E},
    {NOTE_A4, MOZ_E}, {NOTE_FS4, MOZ_E}, {NOTE_A4, MOZ_E}, {NOTE_D4, MOZ_Q},  {NOTE_REST, MOZ_R},
    {NOTE_G4, MOZ_E}, {NOTE_D5, MOZ_E},  {NOTE_G4, MOZ_E}, {NOTE_D5, MOZ_E},  {NOTE_G4, MOZ_E},
    {NOTE_D5, MOZ_E}, {NOTE_G4, MOZ_E},  {NOTE_B4, MOZ_E}, {NOTE_D5, MOZ_Q},  {NOTE_REST, MOZ_R2},
    {NOTE_G4, MOZ_E}, {NOTE_B4, MOZ_E},  {NOTE_D5, MOZ_E}, {NOTE_G5, MOZ_DH},
};

static const chime_note_t k_ode[] = {
    // Beethoven 9 / Ode to Joy, first 8-bar period. ~7.0 s. Played from E5
    // so the small PhotoPainter speaker can render it.
    {NOTE_E5, ODE_Q}, {NOTE_E5, ODE_Q}, {NOTE_F5, ODE_Q},  {NOTE_G5, ODE_Q}, {NOTE_G5, ODE_Q},
    {NOTE_F5, ODE_Q}, {NOTE_E5, ODE_Q}, {NOTE_D5, ODE_Q},  {NOTE_C5, ODE_Q}, {NOTE_C5, ODE_Q},
    {NOTE_D5, ODE_Q}, {NOTE_E5, ODE_Q}, {NOTE_E5, ODE_DH}, {NOTE_D5, ODE_E}, {NOTE_D5, ODE_H},
    {NOTE_E5, ODE_Q}, {NOTE_E5, ODE_Q}, {NOTE_F5, ODE_Q},  {NOTE_G5, ODE_Q}, {NOTE_G5, ODE_Q},
    {NOTE_F5, ODE_Q}, {NOTE_E5, ODE_Q}, {NOTE_D5, ODE_Q},  {NOTE_C5, ODE_Q}, {NOTE_C5, ODE_Q},
    {NOTE_D5, ODE_Q}, {NOTE_E5, ODE_Q}, {NOTE_D5, ODE_DH}, {NOTE_C5, ODE_E}, {NOTE_C5, ODE_H},
};

static const chime_note_t k_frere[] = {
    // Frère Jacques / Vader Jacob opening (two phrases). ~4.0 s.
    {NOTE_C5, FOLK_Q}, {NOTE_D5, FOLK_Q}, {NOTE_E5, FOLK_Q}, {NOTE_C5, FOLK_Q}, {NOTE_C5, FOLK_Q},
    {NOTE_D5, FOLK_Q}, {NOTE_E5, FOLK_Q}, {NOTE_C5, FOLK_Q}, {NOTE_E5, FOLK_Q}, {NOTE_F5, FOLK_Q},
    {NOTE_G5, FOLK_H}, {NOTE_E5, FOLK_Q}, {NOTE_F5, FOLK_Q}, {NOTE_G5, FOLK_H},
};

static const chime_note_t k_twinkle[] = {
    // Twinkle Twinkle Little Star first phrase. ~4.0 s.
    {NOTE_C5, FOLK_Q}, {NOTE_C5, FOLK_Q}, {NOTE_G5, FOLK_Q}, {NOTE_G5, FOLK_Q}, {NOTE_A5, FOLK_Q},
    {NOTE_A5, FOLK_Q}, {NOTE_G5, FOLK_H}, {NOTE_F5, FOLK_Q}, {NOTE_F5, FOLK_Q}, {NOTE_E5, FOLK_Q},
    {NOTE_E5, FOLK_Q}, {NOTE_D5, FOLK_Q}, {NOTE_D5, FOLK_Q}, {NOTE_C5, FOLK_H},
};

static const chime_note_t k_fanfare[] = {
    // Heroic 1-3-5-8 flourish ending on tonic. ~2.3 s.
    {NOTE_C4, FAN_E}, {NOTE_E4, FAN_E}, {NOTE_G4, FAN_E}, {NOTE_C5, FAN_Q}, {NOTE_C5, FAN_E},
    {NOTE_E5, FAN_E}, {NOTE_G5, FAN_E}, {NOTE_C6, FAN_Q}, {NOTE_G5, FAN_E}, {NOTE_E5, FAN_E},
    {NOTE_C5, FAN_E}, {NOTE_C4, FAN_E}, {NOTE_C5, FAN_H},
};

static const chime_note_t k_dingdong[] = {
    // Two-tone doorbell, repeated. ~3.1 s.
    {NOTE_G5, 550}, {NOTE_C5, 850}, {NOTE_REST, 220}, {NOTE_G5, 550}, {NOTE_C5, 900},
};

static const chime_note_t k_doublebeep[] = {
    {NOTE_A5, 160},
    {NOTE_REST, 120},
    {NOTE_A5, 200},
};

static const chime_note_t k_softping[] = {
    {NOTE_A5, 300},
};

static const chime_note_t k_alert[] = {
    {NOTE_A5, 150}, {NOTE_F5, 150}, {NOTE_A5, 150}, {NOTE_F5, 150},  {NOTE_A5, 150},
    {NOTE_F5, 150}, {NOTE_A5, 150}, {NOTE_F5, 150}, {NOTE_REST, 80}, {NOTE_A5, 150},
    {NOTE_F5, 150}, {NOTE_A5, 150}, {NOTE_F5, 150}, {NOTE_A5, 180},  {NOTE_F5, 400},
};

typedef struct {
    const char *id;
    const chime_note_t *notes;
    size_t count;
} chime_preset_entry_t;

static const chime_preset_entry_t k_presets[] = {
    {"triad", k_triad, sizeof(k_triad) / sizeof(k_triad[0])},
    {"mozart", k_mozart, sizeof(k_mozart) / sizeof(k_mozart[0])},
    {"ode", k_ode, sizeof(k_ode) / sizeof(k_ode[0])},
    {"frere", k_frere, sizeof(k_frere) / sizeof(k_frere[0])},
    {"twinkle", k_twinkle, sizeof(k_twinkle) / sizeof(k_twinkle[0])},
    {"fanfare", k_fanfare, sizeof(k_fanfare) / sizeof(k_fanfare[0])},
    {"dingdong", k_dingdong, sizeof(k_dingdong) / sizeof(k_dingdong[0])},
    {"doublebeep", k_doublebeep, sizeof(k_doublebeep) / sizeof(k_doublebeep[0])},
    {"ascending", k_fanfare, sizeof(k_fanfare) / sizeof(k_fanfare[0])},
    {"softping", k_softping, sizeof(k_softping) / sizeof(k_softping[0])},
    {"alert", k_alert, sizeof(k_alert) / sizeof(k_alert[0])},
};

static const char *const k_preset_ids[] = {
    "triad",    "mozart",     "ode",       "frere",    "twinkle", "fanfare",
    "dingdong", "doublebeep", "ascending", "softping", "alert",
};

bool chime_preset_id_is_valid(const char *preset)
{
    if (!preset || !preset[0]) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_preset_ids) / sizeof(k_preset_ids[0]); i++) {
        if (strcmp(preset, k_preset_ids[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool chime_preset_lookup(const char *preset, const chime_note_t **notes, size_t *count)
{
    if (!notes || !count) {
        return false;
    }
    const char *name = (preset && preset[0]) ? preset : CHIME_PRESET_DEFAULT;
    for (size_t i = 0; i < sizeof(k_presets) / sizeof(k_presets[0]); i++) {
        if (strcmp(name, k_presets[i].id) == 0) {
            *notes = k_presets[i].notes;
            *count = k_presets[i].count;
            return true;
        }
    }
    return false;
}

int chime_preset_duration_ms(const char *preset)
{
    const chime_note_t *notes = NULL;
    size_t count = 0;
    if (!chime_preset_lookup(preset, &notes, &count) || !notes) {
        return -1;
    }
    int total = 0;
    for (size_t i = 0; i < count; i++) {
        total += notes[i].duration_ms;
    }
    return total;
}

const char *const *chime_preset_ids(size_t *count)
{
    if (count) {
        *count = sizeof(k_preset_ids) / sizeof(k_preset_ids[0]);
    }
    return k_preset_ids;
}
