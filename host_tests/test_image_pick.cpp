// Random rotation picks during the album walk (#165): every image equally
// likely, the image on the panel only when there is nothing else, and an image
// that just failed to display never.

#include <gtest/gtest.h>

#include <map>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include "image_pick.h"
}

namespace
{

std::vector<std::string> album(int n)
{
    std::vector<std::string> paths;
    for (int i = 0; i < n; i++) {
        paths.push_back("/sdcard/images/Album/photo" + std::to_string(i) + ".epdgz");
    }
    return paths;
}

const char *pick_from(image_pick_t *pick, const std::vector<std::string> &paths, std::mt19937 &rng,
                      const char *avoid = nullptr, const char *exclude = nullptr)
{
    image_pick_init(pick, avoid, exclude);
    for (const auto &p : paths) {
        image_pick_offer(pick, p.c_str(), rng());
    }
    return image_pick_finish(pick);
}

TEST(ImagePick, NothingOfferedPicksNothing)
{
    image_pick_t pick;
    image_pick_init(&pick, nullptr, nullptr);
    EXPECT_EQ(image_pick_finish(&pick), nullptr);
}

TEST(ImagePick, EveryImageIsEquallyLikely)
{
    const auto paths = album(10);
    std::mt19937 rng(1);
    image_pick_t pick;
    std::map<std::string, int> counts;
    const int trials = 100000;
    for (int t = 0; t < trials; t++) {
        counts[pick_from(&pick, paths, rng)]++;
    }
    ASSERT_EQ(counts.size(), paths.size());
    for (const auto &[path, count] : counts) {
        // Expected 10000 each; 5 standard deviations is about 475
        EXPECT_NEAR(count, trials / 10, 500) << path;
    }
}

TEST(ImagePick, NeverRepeatsTheImageOnThePanel)
{
    const auto paths = album(5);
    std::mt19937 rng(2);
    image_pick_t pick;
    std::map<std::string, int> counts;
    for (int t = 0; t < 50000; t++) {
        counts[pick_from(&pick, paths, rng, paths[2].c_str())]++;
    }
    EXPECT_EQ(counts.count(paths[2]), 0u);
    EXPECT_EQ(counts.size(), 4u);
    for (const auto &[path, count] : counts) {
        EXPECT_NEAR(count, 50000 / 4, 500) << path;
    }
}

TEST(ImagePick, RepeatsTheImageOnThePanelWhenItIsTheOnlyOne)
{
    const auto paths = album(1);
    std::mt19937 rng(3);
    image_pick_t pick;
    // A separate buffer, as the caller's last-shown path is
    std::string last = paths[0];
    EXPECT_STREQ(pick_from(&pick, paths, rng, last.c_str()), paths[0].c_str());
}

TEST(ImagePick, NeverPicksAnExcludedImage)
{
    const auto paths = album(3);
    std::mt19937 rng(4);
    image_pick_t pick;
    for (int t = 0; t < 1000; t++) {
        const char *got = pick_from(&pick, paths, rng, paths[0].c_str(), paths[1].c_str());
        ASSERT_STREQ(got, paths[2].c_str());
    }
    // Excluding the only other image falls back to the one on the panel...
    const std::vector<std::string> two(paths.begin(), paths.begin() + 2);
    EXPECT_STREQ(pick_from(&pick, two, rng, paths[0].c_str(), paths[1].c_str()), paths[0].c_str());
    // ...and excluding everything picks nothing
    const std::vector<std::string> one(paths.begin(), paths.begin() + 1);
    EXPECT_EQ(pick_from(&pick, one, rng, nullptr, paths[0].c_str()), nullptr);
}

TEST(ImagePick, EmptyAvoidAndExcludeMeanNone)
{
    // The caller passes its last-shown buffer even when nothing was shown yet
    const auto paths = album(1);
    std::mt19937 rng(5);
    image_pick_t pick;
    EXPECT_STREQ(pick_from(&pick, paths, rng, "", ""), paths[0].c_str());
}

TEST(ImagePick, ThousandsOfImagesNeedNoMoreMemory)
{
    // The pick is one fixed-size struct, whatever the album size
    const auto paths = album(5000);
    std::mt19937 rng(6);
    image_pick_t pick;
    const char *got = pick_from(&pick, paths, rng, paths[0].c_str());
    ASSERT_NE(got, nullptr);
    EXPECT_STRNE(got, paths[0].c_str());
    EXPECT_EQ(pick.candidates, 4999u);
}

}  // namespace
