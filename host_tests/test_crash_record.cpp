// The last-crash record: the reason picked from a core dump summary, and the
// one-line form that is logged and pasted into bug reports.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include "crash_record.h"
}

namespace
{

std::string format(const crash_record_t &rec)
{
    char line[CRASH_RECORD_LINE_MAX];
    crash_record_format(&rec, "seeedstudio_xiao_ee02", line, sizeof(line));
    return line;
}

crash_record_t sample()
{
    crash_record_t rec = {};
    rec.version = CRASH_RECORD_VERSION;
    rec.pc = 0x4201a2b3;
    rec.bt[0] = 0x4201a2b3;
    rec.bt[1] = 0x4201c3d4;
    rec.bt_depth = 2;
    rec.dump_size = 23456;
    rec.found_at = 1790000000;  // 2026-09-21T14:13:20Z
    strcpy(rec.task, "httpd");
    strcpy(rec.firmware, "v2.20.1");
    strcpy(rec.elf_sha, "1a2b3c4d");
    crash_record_set_reason(&rec, nullptr, 28, 0);
    return rec;
}

TEST(CrashRecord, PanicDetailsWinAndBecomeOneLine)
{
    crash_record_t rec = {};
    crash_record_set_reason(&rec, "  assert failed: foo bar.c:12\n (x != 0)\r\n", 28, 0);
    EXPECT_STREQ(rec.reason, "assert failed: foo bar.c:12 (x != 0)");
}

TEST(CrashRecord, ExceptionCauseWithoutDetails)
{
    crash_record_t rec = {};
    crash_record_set_reason(&rec, "", 28, 0x10);
    EXPECT_STREQ(rec.reason, "LoadProhibited, vaddr 0x00000010");
    crash_record_set_reason(&rec, nullptr, 29, 0);
    EXPECT_STREQ(rec.reason, "StoreProhibited, vaddr 0x00000000");
    crash_record_set_reason(&rec, " \n", 0, 0);
    EXPECT_STREQ(rec.reason, "IllegalInstruction, vaddr 0x00000000");
}

TEST(CrashRecord, PseudoAndUnknownCauses)
{
    crash_record_t rec = {};
    crash_record_set_reason(&rec, nullptr, 64 + 5, 0);
    EXPECT_STREQ(rec.reason, "Interrupt wdt timeout on CPU0");
    crash_record_set_reason(&rec, nullptr, 64 + 7, 0);
    EXPECT_STREQ(rec.reason, "Cache error");
    crash_record_set_reason(&rec, nullptr, 10, 0);
    EXPECT_STREQ(rec.reason, "EXCCAUSE 10");
    crash_record_set_reason(&rec, nullptr, 0xffff, 0);
    EXPECT_STREQ(rec.reason, "EXCCAUSE 65535");
}

TEST(CrashRecord, LongDetailsAreTruncated)
{
    crash_record_t rec = {};
    std::string details(300, 'x');
    crash_record_set_reason(&rec, details.c_str(), 0, 0);
    EXPECT_EQ(strlen(rec.reason), sizeof(rec.reason) - 1);
}

TEST(CrashRecord, FormatsEverythingOnOneLine)
{
    EXPECT_EQ(format(sample()),
              "LoadProhibited, vaddr 0x00000000 | task httpd, pc 0x4201a2b3, "
              "bt 0x4201a2b3 0x4201c3d4 | fw v2.20.1, elf 1a2b3c4d, "
              "board seeedstudio_xiao_ee02 | found 2026-09-21T14:13:20Z | dump 23456 B");
}

TEST(CrashRecord, MarksUnknownClockAndBuild)
{
    crash_record_t rec = sample();
    rec.found_at = 0;
    rec.firmware[0] = '\0';
    rec.bt_corrupted = true;
    std::string line = format(rec);
    EXPECT_NE(line.find("bt 0x4201a2b3 0x4201c3d4 (corrupted) |"), std::string::npos) << line;
    EXPECT_NE(line.find("fw unknown, elf 1a2b3c4d"), std::string::npos) << line;
    EXPECT_NE(line.find("found clock not set"), std::string::npos) << line;
}

TEST(CrashRecord, WorstCaseFitsAndSmallBuffersStayTerminated)
{
    crash_record_t rec = sample();
    memset(rec.reason, 'r', sizeof(rec.reason) - 1);
    memset(rec.task, 't', sizeof(rec.task) - 1);
    memset(rec.firmware, 'f', sizeof(rec.firmware) - 1);
    rec.bt_depth = CRASH_RECORD_BT_MAX;
    rec.bt_corrupted = true;
    rec.dump_size = 0xffffffff;
    std::string line = format(rec);
    EXPECT_LT(line.size(), (size_t) CRASH_RECORD_LINE_MAX - 1);
    EXPECT_NE(line.find(" | dump 4294967295 B"), std::string::npos);

    char small[16];
    crash_record_format(&rec, "board", small, sizeof(small));
    EXPECT_EQ(strlen(small), sizeof(small) - 1);
}

}  // namespace
