// Tests for the .current.* / thumbnail lifecycle shared by the display
// flows (display_flow.c), including what /api/current_image ends up serving
// after each flow's disposal. FS_MOUNT_POINT is redirected to a local
// directory (see CMakeLists), so every CURRENT_*_PATH lands in
// pf_storage/ under the test's working directory.

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>

extern "C" {
#include "config.h"
#include "display_flow.h"

extern bool test_storage_persistent;
}

namespace
{

const char *kStorageDir = FS_MOUNT_POINT;

void RemoveTree(const std::string &dir)
{
    std::string cmd = "rm -rf '" + dir + "'";
    system(cmd.c_str());
}

void Touch(const std::string &path, const std::string &content)
{
    std::ofstream f(path, std::ios::binary);
    f << content;
}

bool Exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::string ReadAll(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void WriteLink(const std::string &displayed_name)
{
    // Same format as display_manager's create_image_link
    Touch(CURRENT_IMAGE_LINK, displayed_name);
}

// What /api/current_image would send: body + content type
struct Served {
    bool ok = false;
    std::string content;
    std::string type;
};

Served ServeCurrent()
{
    const char *type = nullptr;
    FILE *fp = display_flow_open_current(&type);
    if (!fp)
        return {};
    Served out;
    out.ok = true;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        out.content.append(buf, n);
    fclose(fp);
    out.type = type ? type : "";
    return out;
}

class DisplayFlowTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        RemoveTree(kStorageDir);
        ASSERT_EQ(mkdir(kStorageDir, 0755), 0);
        test_storage_persistent = true;
    }

    void TearDown() override
    {
        RemoveTree(kStorageDir);
    }

    std::string Upload(const std::string &content = "original")
    {
        Touch(CURRENT_UPLOAD_PATH, content);
        return CURRENT_UPLOAD_PATH;
    }
};

// --- display_flow_stage_file ----------------------------------------------

TEST_F(DisplayFlowTest, StageFileMovesBmpIntoSlot)
{
    std::string src = Upload("bmp-bytes");
    const char *staged = display_flow_stage_file(src.c_str(), IMAGE_FORMAT_BMP);
    ASSERT_STREQ(staged, CURRENT_BMP_PATH);
    EXPECT_FALSE(Exists(src));
    EXPECT_EQ(ReadAll(CURRENT_BMP_PATH), "bmp-bytes");
}

TEST_F(DisplayFlowTest, StageFileMovesEpdgzIntoSlot)
{
    std::string src = Upload("epd-bytes");
    const char *staged = display_flow_stage_file(src.c_str(), IMAGE_FORMAT_EPD_GZ);
    ASSERT_STREQ(staged, CURRENT_EPD_PATH);
    EXPECT_EQ(ReadAll(CURRENT_EPD_PATH), "epd-bytes");
}

TEST_F(DisplayFlowTest, StageFileReplacesPreviousSlotContent)
{
    Touch(CURRENT_BMP_PATH, "old");
    std::string src = Upload("new");
    ASSERT_NE(display_flow_stage_file(src.c_str(), IMAGE_FORMAT_BMP), nullptr);
    EXPECT_EQ(ReadAll(CURRENT_BMP_PATH), "new");
}

TEST_F(DisplayFlowTest, StageFileFailureConsumesSource)
{
    // Occupy the slot with a directory so the rename must fail
    ASSERT_EQ(mkdir(CURRENT_BMP_PATH, 0755), 0);
    std::string src = Upload();
    EXPECT_EQ(display_flow_stage_file(src.c_str(), IMAGE_FORMAT_BMP), nullptr);
    EXPECT_FALSE(Exists(src));
}

// --- display_flow_retire_source -------------------------------------------

TEST_F(DisplayFlowTest, RetireJpgKeepsOriginalAsThumbnail)
{
    Touch(CURRENT_PNG_PATH, "stale-png");
    Touch(CURRENT_JPG_PATH, "stale-thumb");
    Touch(CURRENT_BMP_PATH, "stale-bmp");
    Touch(CURRENT_EPD_PATH, "stale-epd");
    std::string src = Upload("jpeg-original");

    display_flow_retire_source(src.c_str(), IMAGE_FORMAT_JPG, false);

    EXPECT_EQ(ReadAll(CURRENT_JPG_PATH), "jpeg-original");
    EXPECT_FALSE(Exists(src));
    EXPECT_FALSE(Exists(CURRENT_PNG_PATH));
    EXPECT_FALSE(Exists(CURRENT_BMP_PATH));
    EXPECT_FALSE(Exists(CURRENT_EPD_PATH));
}

TEST_F(DisplayFlowTest, RetireJpgWithFreshThumbnailDropsOriginal)
{
    Touch(CURRENT_JPG_PATH, "fresh-thumb");
    std::string src = Upload("jpeg-original");

    display_flow_retire_source(src.c_str(), IMAGE_FORMAT_JPG, true);

    EXPECT_EQ(ReadAll(CURRENT_JPG_PATH), "fresh-thumb");
    EXPECT_FALSE(Exists(src));
}

TEST_F(DisplayFlowTest, RetirePngKeepsOriginalForCurrentImage)
{
    Touch(CURRENT_JPG_PATH, "stale-thumb");
    std::string src = Upload("png-original");

    display_flow_retire_source(src.c_str(), IMAGE_FORMAT_PNG, false);

    EXPECT_EQ(ReadAll(CURRENT_PNG_PATH), "png-original");
    EXPECT_FALSE(Exists(CURRENT_JPG_PATH));  // stale thumbnail dropped
}

TEST_F(DisplayFlowTest, RetirePngWithFreshThumbnailKeepsBoth)
{
    Touch(CURRENT_JPG_PATH, "fresh-thumb");
    std::string src = Upload("png-original");

    display_flow_retire_source(src.c_str(), IMAGE_FORMAT_PNG, true);

    EXPECT_EQ(ReadAll(CURRENT_PNG_PATH), "png-original");
    EXPECT_EQ(ReadAll(CURRENT_JPG_PATH), "fresh-thumb");
}

TEST_F(DisplayFlowTest, RetireOnMemFsRetainsNothingFullSize)
{
    test_storage_persistent = false;
    Touch(CURRENT_PNG_PATH, "stale-png");
    Touch(CURRENT_JPG_PATH, "fresh-thumb");
    std::string src = Upload("png-original");

    display_flow_retire_source(src.c_str(), IMAGE_FORMAT_PNG, true);

    EXPECT_FALSE(Exists(src));
    EXPECT_FALSE(Exists(CURRENT_PNG_PATH));
    // Only the small fresh thumbnail survives on MemFS
    EXPECT_EQ(ReadAll(CURRENT_JPG_PATH), "fresh-thumb");
}

TEST_F(DisplayFlowTest, RetireOnMemFsDropsStaleThumbnail)
{
    test_storage_persistent = false;
    Touch(CURRENT_JPG_PATH, "stale-thumb");
    std::string src = Upload();

    display_flow_retire_source(src.c_str(), IMAGE_FORMAT_JPG, false);

    EXPECT_FALSE(Exists(CURRENT_JPG_PATH));
}

// --- display_flow_drop_stale_current --------------------------------------

TEST_F(DisplayFlowTest, DropStaleKeepsDisplayedFileAndThumbnail)
{
    Touch(CURRENT_PNG_PATH, "png");
    Touch(CURRENT_BMP_PATH, "bmp");
    Touch(CURRENT_EPD_PATH, "epd");
    Touch(CURRENT_JPG_PATH, "thumb");

    display_flow_drop_stale_current(CURRENT_BMP_PATH, true);

    EXPECT_TRUE(Exists(CURRENT_BMP_PATH));
    EXPECT_TRUE(Exists(CURRENT_JPG_PATH));
    EXPECT_FALSE(Exists(CURRENT_PNG_PATH));
    EXPECT_FALSE(Exists(CURRENT_EPD_PATH));
}

TEST_F(DisplayFlowTest, DropStaleWithoutThumbnailDropsIt)
{
    Touch(CURRENT_EPD_PATH, "epd");
    Touch(CURRENT_JPG_PATH, "thumb");

    display_flow_drop_stale_current(CURRENT_EPD_PATH, false);

    EXPECT_TRUE(Exists(CURRENT_EPD_PATH));
    EXPECT_FALSE(Exists(CURRENT_JPG_PATH));
}

TEST_F(DisplayFlowTest, DropStaleOnMemFsKeepsNoImageFile)
{
    test_storage_persistent = false;
    Touch(CURRENT_BMP_PATH, "bmp");
    Touch(CURRENT_JPG_PATH, "thumb");

    display_flow_drop_stale_current(CURRENT_BMP_PATH, true);

    EXPECT_FALSE(Exists(CURRENT_BMP_PATH));
    EXPECT_TRUE(Exists(CURRENT_JPG_PATH));  // thumbnail kept when requested
}

// --- display_flow_open_current (what /api/current_image serves) ----------

TEST_F(DisplayFlowTest, ServeCurrentPrefersThumbnailSibling)
{
    Touch(CURRENT_PNG_PATH, "png-original");
    Touch(CURRENT_JPG_PATH, "thumb");
    WriteLink(CURRENT_PNG_PATH);

    Served s = ServeCurrent();
    ASSERT_TRUE(s.ok);
    EXPECT_EQ(s.content, "thumb");
    EXPECT_EQ(s.type, "image/jpeg");
}

TEST_F(DisplayFlowTest, ServeCurrentFallsBackToPngOriginal)
{
    Touch(CURRENT_PNG_PATH, "png-original");
    WriteLink(CURRENT_PNG_PATH);

    Served s = ServeCurrent();
    ASSERT_TRUE(s.ok);
    EXPECT_EQ(s.content, "png-original");
    EXPECT_EQ(s.type, "image/png");
}

TEST_F(DisplayFlowTest, ServeCurrentFallsBackToBmpOriginal)
{
    Touch(CURRENT_BMP_PATH, "bmp-original");
    WriteLink(CURRENT_BMP_PATH);

    Served s = ServeCurrent();
    ASSERT_TRUE(s.ok);
    EXPECT_EQ(s.content, "bmp-original");
    EXPECT_EQ(s.type, "image/bmp");
}

TEST_F(DisplayFlowTest, ServeCurrentNeverServesRawEpdgz)
{
    Touch(CURRENT_EPD_PATH, "raw-panel-bytes");
    WriteLink(CURRENT_EPD_PATH);

    EXPECT_FALSE(ServeCurrent().ok);
}

TEST_F(DisplayFlowTest, ServeCurrentServesAlbumEpdgzThumbnail)
{
    // Album save: link points at the album .epdgz whose .jpg sibling is the
    // staged preview
    std::string album = std::string(kStorageDir) + "/images";
    mkdir(album.c_str(), 0755);
    album += "/Downloads";
    mkdir(album.c_str(), 0755);
    std::string image = album + "/download_42.epdgz";
    std::string thumb = album + "/download_42.jpg";
    Touch(image, "epd");
    Touch(thumb, "preview");
    WriteLink(image);

    Served s = ServeCurrent();
    ASSERT_TRUE(s.ok);
    EXPECT_EQ(s.content, "preview");
    EXPECT_EQ(s.type, "image/jpeg");
}

TEST_F(DisplayFlowTest, ServeCurrentNoLinkReportsNothing)
{
    EXPECT_FALSE(ServeCurrent().ok);
}

TEST_F(DisplayFlowTest, ServeCurrentMissingFilesReportsNothing)
{
    WriteLink(CURRENT_PNG_PATH);  // link exists, files do not
    EXPECT_FALSE(ServeCurrent().ok);
}

// --- End-to-end flow simulations ------------------------------------------
// Each simulates one display flow's disposal + publish and asserts the API
// would serve the right thing afterwards.

TEST_F(DisplayFlowTest, JpgDirectDisplayServesOriginalAsThumbnail)
{
    // display_received_image: stream succeeded, no uploaded thumbnail
    std::string src = Upload("jpeg-original");
    display_flow_retire_source(src.c_str(), IMAGE_FORMAT_JPG, false);
    WriteLink(CURRENT_JPG_PATH);  // pub.display_name for a JPG source

    Served s = ServeCurrent();
    ASSERT_TRUE(s.ok);
    EXPECT_EQ(s.content, "jpeg-original");
    EXPECT_EQ(s.type, "image/jpeg");
}

TEST_F(DisplayFlowTest, PngDirectDisplayWithThumbnailServesIt)
{
    // display_received_image: stream succeeded, multipart thumbnail provided
    std::string src = Upload("png-original");
    display_flow_retire_source(src.c_str(), IMAGE_FORMAT_PNG, true);
    Touch(CURRENT_JPG_PATH, "uploaded-thumb");  // staged after retire
    WriteLink(CURRENT_PNG_PATH);

    Served s = ServeCurrent();
    ASSERT_TRUE(s.ok);
    EXPECT_EQ(s.content, "uploaded-thumb");
    EXPECT_EQ(s.type, "image/jpeg");
}

TEST_F(DisplayFlowTest, PngDirectDisplayWithoutThumbnailServesOriginal)
{
    std::string src = Upload("png-original");
    display_flow_retire_source(src.c_str(), IMAGE_FORMAT_PNG, false);
    WriteLink(CURRENT_PNG_PATH);

    Served s = ServeCurrent();
    ASSERT_TRUE(s.ok);
    EXPECT_EQ(s.content, "png-original");
    EXPECT_EQ(s.type, "image/png");
}

TEST_F(DisplayFlowTest, UrlFetchWithDownloadedThumbnailServesIt)
{
    // fetch_stream_display, no album: thumbnail downloaded to .current.jpg
    // before display, original retired after
    Touch(CURRENT_JPG_PATH, "server-thumb");
    std::string src = Upload("png-original");
    display_flow_retire_source(src.c_str(), IMAGE_FORMAT_PNG, true);
    WriteLink(CURRENT_PNG_PATH);  // fallback_name published

    Served s = ServeCurrent();
    ASSERT_TRUE(s.ok);
    EXPECT_EQ(s.content, "server-thumb");
}

TEST_F(DisplayFlowTest, EpdgzFileDisplayServesDownloadedThumbnail)
{
    // fetch_display_file, no album: EPDGZ staged + displayed, downloaded
    // thumbnail kept
    Touch(CURRENT_JPG_PATH, "server-thumb");
    std::string src = Upload("epd-bytes");
    const char *staged = display_flow_stage_file(src.c_str(), IMAGE_FORMAT_EPD_GZ);
    ASSERT_NE(staged, nullptr);
    WriteLink(staged);  // display_manager_show_image records the path
    display_flow_drop_stale_current(staged, true);

    Served s = ServeCurrent();
    ASSERT_TRUE(s.ok);
    EXPECT_EQ(s.content, "server-thumb");
}

TEST_F(DisplayFlowTest, EpdgzFileDisplayWithoutThumbnailHasNoServableImage)
{
    std::string src = Upload("epd-bytes");
    const char *staged = display_flow_stage_file(src.c_str(), IMAGE_FORMAT_EPD_GZ);
    ASSERT_NE(staged, nullptr);
    WriteLink(staged);
    display_flow_drop_stale_current(staged, false);

    EXPECT_FALSE(ServeCurrent().ok);
}

}  // namespace
