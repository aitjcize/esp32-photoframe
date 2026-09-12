#include <gtest/gtest.h>

#include <cstring>
#include <vector>

extern "C" {
#include "chime_presets.h"
}

namespace
{

const chime_note_t *notes_of(const char *id, size_t *count)
{
    const chime_note_t *notes = nullptr;
    *count = 0;
    EXPECT_TRUE(chime_preset_lookup(id, &notes, count));
    return notes;
}

std::vector<float> pitched_freqs(const char *id, size_t max_notes)
{
    size_t count = 0;
    const chime_note_t *notes = notes_of(id, &count);
    std::vector<float> out;
    if (!notes) {
        return out;
    }
    for (size_t i = 0; i < count && out.size() < max_notes; i++) {
        if (notes[i].freq_hz > 0.0f) {
            out.push_back(notes[i].freq_hz);
        }
    }
    return out;
}

}  // namespace

TEST(ChimePresets, DefaultAndFallbackIds)
{
    EXPECT_STREQ(CHIME_PRESET_DEFAULT, "mozart");
    EXPECT_STREQ(CHIME_PRESET_FALLBACK, "triad");
    EXPECT_EQ(CHIME_PRESET_MAX_MS, 8000);
}

TEST(ChimePresets, IdListMatchesValidation)
{
    size_t count = 0;
    const char *const *ids = chime_preset_ids(&count);
    ASSERT_NE(ids, nullptr);
    ASSERT_GE(count, 11u);

    EXPECT_TRUE(chime_preset_id_is_valid("mozart"));
    EXPECT_TRUE(chime_preset_id_is_valid("ode"));
    EXPECT_TRUE(chime_preset_id_is_valid("frere"));
    EXPECT_TRUE(chime_preset_id_is_valid("twinkle"));
    EXPECT_TRUE(chime_preset_id_is_valid("fanfare"));
    EXPECT_TRUE(chime_preset_id_is_valid("triad"));
    EXPECT_TRUE(chime_preset_id_is_valid("dingdong"));
    EXPECT_TRUE(chime_preset_id_is_valid("doublebeep"));
    EXPECT_TRUE(chime_preset_id_is_valid("ascending"));
    EXPECT_TRUE(chime_preset_id_is_valid("softping"));
    EXPECT_TRUE(chime_preset_id_is_valid("alert"));

    EXPECT_FALSE(chime_preset_id_is_valid(nullptr));
    EXPECT_FALSE(chime_preset_id_is_valid(""));
    EXPECT_FALSE(chime_preset_id_is_valid("mario"));
    EXPECT_FALSE(chime_preset_id_is_valid("not-a-tone"));

    for (size_t i = 0; i < count; i++) {
        EXPECT_TRUE(chime_preset_id_is_valid(ids[i])) << ids[i];
        EXPECT_LT(std::strlen(ids[i]), 16u) << ids[i];
    }
}

TEST(ChimePresets, EmptyLookupUsesMozart)
{
    size_t count = 0;
    const chime_note_t *empty = nullptr;
    const chime_note_t *mozart = nullptr;
    size_t mozart_count = 0;
    ASSERT_TRUE(chime_preset_lookup("", &empty, &count));
    ASSERT_TRUE(chime_preset_lookup("mozart", &mozart, &mozart_count));
    EXPECT_EQ(empty, mozart);
    EXPECT_EQ(count, mozart_count);
    EXPECT_FALSE(chime_preset_lookup("mario", &empty, &count));
}

TEST(ChimePresets, TuneDurationsAreShortMelodiesNotBeeps)
{
    const int mozart_ms = chime_preset_duration_ms("mozart");
    const int ode_ms = chime_preset_duration_ms("ode");
    const int frere_ms = chime_preset_duration_ms("frere");
    const int twinkle_ms = chime_preset_duration_ms("twinkle");
    const int fanfare_ms = chime_preset_duration_ms("fanfare");
    const int triad_ms = chime_preset_duration_ms("triad");
    const int dingdong_ms = chime_preset_duration_ms("dingdong");

    EXPECT_GE(mozart_ms, 4000);
    EXPECT_LE(mozart_ms, 6000);
    EXPECT_GE(ode_ms, 5000);
    EXPECT_LE(ode_ms, 8000);
    EXPECT_GE(frere_ms, 3500);
    EXPECT_LE(frere_ms, 5000);
    EXPECT_GE(twinkle_ms, 3500);
    EXPECT_LE(twinkle_ms, 5000);
    EXPECT_GE(fanfare_ms, 1800);
    EXPECT_LE(fanfare_ms, 3000);
    EXPECT_GE(triad_ms, 1600);
    EXPECT_LE(triad_ms, 3000);
    EXPECT_GE(dingdong_ms, 2500);
    EXPECT_LE(dingdong_ms, 4000);

    size_t count = 0;
    const char *const *ids = chime_preset_ids(&count);
    for (size_t i = 0; i < count; i++) {
        const int ms = chime_preset_duration_ms(ids[i]);
        EXPECT_GT(ms, 0) << ids[i];
        EXPECT_LE(ms, CHIME_PRESET_MAX_MS) << ids[i];
    }
}

TEST(ChimePresets, MozartOpeningIsEineKleineNachtmusik)
{
    auto freqs = pitched_freqs("mozart", 9);
    ASSERT_EQ(freqs.size(), 9u);
    EXPECT_NEAR(freqs[0], NOTE_G4, 0.05);
    EXPECT_NEAR(freqs[1], NOTE_D5, 0.05);
    EXPECT_NEAR(freqs[2], NOTE_G4, 0.05);
    EXPECT_NEAR(freqs[3], NOTE_D5, 0.05);
    EXPECT_NEAR(freqs[4], NOTE_G4, 0.05);
    EXPECT_NEAR(freqs[5], NOTE_D5, 0.05);
    EXPECT_NEAR(freqs[6], NOTE_G4, 0.05);
    EXPECT_NEAR(freqs[7], NOTE_B4, 0.05);
    EXPECT_NEAR(freqs[8], NOTE_D5, 0.05);
}

TEST(ChimePresets, OdeOpeningIsOdeToJoy)
{
    auto freqs = pitched_freqs("ode", 8);
    ASSERT_EQ(freqs.size(), 8u);
    EXPECT_NEAR(freqs[0], NOTE_E5, 0.05);
    EXPECT_NEAR(freqs[1], NOTE_E5, 0.05);
    EXPECT_NEAR(freqs[2], NOTE_F5, 0.05);
    EXPECT_NEAR(freqs[3], NOTE_G5, 0.05);
    EXPECT_NEAR(freqs[4], NOTE_G5, 0.05);
    EXPECT_NEAR(freqs[5], NOTE_F5, 0.05);
    EXPECT_NEAR(freqs[6], NOTE_E5, 0.05);
    EXPECT_NEAR(freqs[7], NOTE_D5, 0.05);
}

TEST(ChimePresets, FrereOpeningIsFrereJacques)
{
    auto freqs = pitched_freqs("frere", 8);
    ASSERT_EQ(freqs.size(), 8u);
    EXPECT_NEAR(freqs[0], NOTE_C5, 0.05);
    EXPECT_NEAR(freqs[1], NOTE_D5, 0.05);
    EXPECT_NEAR(freqs[2], NOTE_E5, 0.05);
    EXPECT_NEAR(freqs[3], NOTE_C5, 0.05);
    EXPECT_NEAR(freqs[4], NOTE_C5, 0.05);
    EXPECT_NEAR(freqs[5], NOTE_D5, 0.05);
    EXPECT_NEAR(freqs[6], NOTE_E5, 0.05);
    EXPECT_NEAR(freqs[7], NOTE_C5, 0.05);
}

TEST(ChimePresets, TwinkleOpeningIsTwinkleTwinkle)
{
    auto freqs = pitched_freqs("twinkle", 7);
    ASSERT_EQ(freqs.size(), 7u);
    EXPECT_NEAR(freqs[0], NOTE_C5, 0.05);
    EXPECT_NEAR(freqs[1], NOTE_C5, 0.05);
    EXPECT_NEAR(freqs[2], NOTE_G5, 0.05);
    EXPECT_NEAR(freqs[3], NOTE_G5, 0.05);
    EXPECT_NEAR(freqs[4], NOTE_A5, 0.05);
    EXPECT_NEAR(freqs[5], NOTE_A5, 0.05);
    EXPECT_NEAR(freqs[6], NOTE_G5, 0.05);
}

TEST(ChimePresets, FanfareIs1358AndAscendingAliasesIt)
{
    auto freqs = pitched_freqs("fanfare", 4);
    ASSERT_EQ(freqs.size(), 4u);
    EXPECT_NEAR(freqs[0], NOTE_C4, 0.05);
    EXPECT_NEAR(freqs[1], NOTE_E4, 0.05);
    EXPECT_NEAR(freqs[2], NOTE_G4, 0.05);
    EXPECT_NEAR(freqs[3], NOTE_C5, 0.05);
    EXPECT_EQ(chime_preset_duration_ms("ascending"), chime_preset_duration_ms("fanfare"));

    size_t fanfare_count = 0;
    size_t ascending_count = 0;
    const chime_note_t *fanfare = notes_of("fanfare", &fanfare_count);
    const chime_note_t *ascending = notes_of("ascending", &ascending_count);
    EXPECT_EQ(fanfare, ascending);
    EXPECT_EQ(fanfare_count, ascending_count);
}

TEST(ChimePresets, DingdongIsLongTwoToneDoorbell)
{
    size_t count = 0;
    const chime_note_t *notes = notes_of("dingdong", &count);
    ASSERT_GE(count, 2u);
    EXPECT_NEAR(notes[0].freq_hz, NOTE_G5, 0.05);
    EXPECT_GE(notes[0].duration_ms, 400);
    EXPECT_NEAR(notes[1].freq_hz, NOTE_C5, 0.05);
    EXPECT_GE(notes[1].duration_ms, 700);
}

TEST(ChimePresets, NotesAreRestsOrAudibleSineRange)
{
    size_t nids = 0;
    const char *const *ids = chime_preset_ids(&nids);
    for (size_t p = 0; p < nids; p++) {
        size_t count = 0;
        const chime_note_t *notes = notes_of(ids[p], &count);
        ASSERT_GT(count, 0u) << ids[p];
        size_t pitched = 0;
        for (size_t i = 0; i < count; i++) {
            EXPECT_GT(notes[i].duration_ms, 0) << ids[p];
            if (notes[i].freq_hz == 0.0f) {
                continue;
            }
            EXPECT_GE(notes[i].freq_hz, 190.0f) << ids[p];
            EXPECT_LE(notes[i].freq_hz, 1100.0f) << ids[p];
            pitched++;
        }
        EXPECT_GE(pitched, 1u) << ids[p];
    }
}
