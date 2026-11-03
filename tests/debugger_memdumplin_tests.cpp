/*
 *  MEMDUMPLIN (src/debug/debug.cpp): the file name it writes, and that it refuses a dump
 *  that wraps the 32-bit linear space. Reading the guest's pages needs a machine with RAM
 *  and page tables, which a unit test has not got: there it dumps nothing and says so.
 */

#if C_DEBUG

#include <cstdio>
#include <string>

#include "cross.h"
#include "gtest/gtest.h"

/* debug.h is not included before tests.h is compiled inside debug.cpp. */
bool ParseCommand(char *str);

namespace {

bool MemDumpLinCommand(const std::string &text) {
    std::string copy(text);
    return ParseCommand(&copy[0]);
}

/* The exact name a file has in its directory: a lookup by name answers for any case on a
   case-insensitive volume, a listing does not. */
bool Lists(const std::string &dir, const std::string &name) {
    dir_information *d = open_directory(dir.c_str());
    if (d == NULL) return false;
    char entry[CROSS_LEN], short_name[CROSS_LEN];
    bool is_dir = false, found = false;
    for (bool more = read_directory_first(d, entry, short_name, is_dir); more;
         more = read_directory_next(d, entry, short_name, is_dir))
        if (name == entry) found = true;
    close_directory(d);
    return found;
}

class DebuggerMemDumpLin : public ::testing::Test {
protected:
    void SetUp() override {
        /* A file name with a directory in it reads as a division to the expression parser the
           length goes through, so the files are in the working directory. */
        dir = "";
        std::remove((dir + "MixedCase.Bin").c_str());
        std::remove((dir + "MIXEDCASE.BIN").c_str());
        std::remove((dir + "Wrapped.Bin").c_str());
        std::remove((dir + "WRAPPED.BIN").c_str());
    }
    void TearDown() override {
        std::remove((dir + "MixedCase.Bin").c_str());
        std::remove((dir + "MIXEDCASE.BIN").c_str());
        std::remove((dir + "Wrapped.Bin").c_str());
        std::remove((dir + "WRAPPED.BIN").c_str());
    }
    std::string dir;
};

TEST_F(DebuggerMemDumpLin, WritesTheFileUnderTheNameAsTyped) {
    EXPECT_TRUE(MemDumpLinCommand("MEMDUMPLIN 1000 4 " + dir + "MixedCase.Bin"));
    EXPECT_TRUE(Lists(".", "MixedCase.Bin")) << "ParseCommand upper-cases its own copy of the line";
    EXPECT_FALSE(Lists(".", "MIXEDCASE.BIN"));
}

TEST_F(DebuggerMemDumpLin, WillNotDumpAcrossTheEndOfTheLinearSpace) {
    EXPECT_TRUE(MemDumpLinCommand("MEMDUMPLIN FFFFFF00 200 " + dir + "Wrapped.Bin"));
    EXPECT_FALSE(Lists(".", "Wrapped.Bin") || Lists(".", "WRAPPED.BIN"))
        << "the run would continue from linear 0";
}

} // namespace

#endif
