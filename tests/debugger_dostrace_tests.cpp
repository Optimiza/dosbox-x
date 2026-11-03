/*
 *  DOSTRACE (src/debug/debug.cpp, src/dos/dos.cpp): the status it prints, the file it opens,
 *  and the refusals. The rows of an INT 21h call need a running guest, which a unit test has not.
 */

#if C_DEBUG

#include <cstdio>
#include <string>
#include <vector>

#include "gtest/gtest.h"

/* debug.h, which declares ParseCommand's neighbours, is not included before tests.h is
   compiled inside debug.cpp. */
bool ParseCommand(char *str);
int DOS_TraceCommand(const char *path, std::vector<std::string> &lines);

namespace {

class DebuggerDosTrace : public ::testing::Test {
protected:
    void SetUp() override { path = std::string(::testing::TempDir()) + "dostrace_test.txt"; }
    void TearDown() override {
        std::vector<std::string> ignored;
        DOS_TraceCommand("", ignored);
        std::remove(path.c_str());
    }
    std::string path;
};

TEST_F(DebuggerDosTrace, SaysItIsOffBeforeAnyTrace) {
    std::vector<std::string> lines;
    EXPECT_EQ(DOS_TraceCommand(NULL, lines), 1);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "DEBUG: DOSTRACE OFF CALLS=0 ROWS=0 DROPPED=0 FILE=-");
}

TEST_F(DebuggerDosTrace, StartsIntoANewFileAndNamesIt) {
    std::vector<std::string> lines;
    ASSERT_EQ(DOS_TraceCommand(path.c_str(), lines), 1);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "DEBUG: DOSTRACE ON CALLS=0 ROWS=0 DROPPED=0 FILE=" + path);
    FILE *file = fopen(path.c_str(), "r");
    ASSERT_NE(file, nullptr) << "the file exists as soon as the trace starts";
    fclose(file);
}

TEST_F(DebuggerDosTrace, WillNotStartASecondTraceOverTheFirst) {
    std::vector<std::string> lines;
    ASSERT_EQ(DOS_TraceCommand(path.c_str(), lines), 1);
    lines.clear();
    EXPECT_EQ(DOS_TraceCommand((path + ".2").c_str(), lines), -1);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "DEBUG: DOSTRACE is already on: turn it OFF first");
}

TEST_F(DebuggerDosTrace, TurnsOffAndSaysSo) {
    std::vector<std::string> lines;
    ASSERT_EQ(DOS_TraceCommand(path.c_str(), lines), 1);
    lines.clear();
    EXPECT_EQ(DOS_TraceCommand("", lines), 1);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "DEBUG: DOSTRACE OFF CALLS=0 ROWS=0 DROPPED=0 FILE=-");
}

TEST_F(DebuggerDosTrace, SaysWhenItCannotWriteTheFile) {
    std::vector<std::string> lines;
    const std::string nowhere = path + "/no/such/directory/trace.txt";
    EXPECT_EQ(DOS_TraceCommand(nowhere.c_str(), lines), -1);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "DEBUG: DOSTRACE cannot write " + nowhere);
}

TEST_F(DebuggerDosTrace, TheCommandKeepsTheFileNameAsTyped) {
    std::string on = "DOSTRACE " + path;
    EXPECT_TRUE(ParseCommand(&on[0]));
    std::vector<std::string> lines;
    ASSERT_EQ(DOS_TraceCommand(NULL, lines), 1);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "DEBUG: DOSTRACE ON CALLS=0 ROWS=0 DROPPED=0 FILE=" + path)
        << "the name keeps its case: the line is upper-cased before it is parsed";
    std::string again = "DOSTRACE " + path + ".2";
    EXPECT_FALSE(ParseCommand(&again[0])) << "already on";
    std::string off = "DOSTRACE off";
    EXPECT_TRUE(ParseCommand(&off[0]));
}

TEST_F(DebuggerDosTrace, TheFileNameMayFollowATabAsWellAsASpace) {
    std::string on = "DOSTRACE\t" + path;
    EXPECT_TRUE(ParseCommand(&on[0]));
    std::vector<std::string> lines;
    ASSERT_EQ(DOS_TraceCommand(NULL, lines), 1);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "DEBUG: DOSTRACE ON CALLS=0 ROWS=0 DROPPED=0 FILE=" + path);
}

} // namespace

#endif
