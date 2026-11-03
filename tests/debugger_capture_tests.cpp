/*
 *  CAPTURE (src/hardware/hardware.cpp, src/debug/debug.cpp): the list it prints, and the
 *  commands it refuses. Turning a capture on or off opens a file, which a unit test does not.
 */

#if C_DEBUG && !defined(C_EMSCRIPTEN)

#include <string>
#include <vector>

#include "gtest/gtest.h"

/* debug.h, which declares ParseCommand's neighbours, has no include guard, and tests.h is
   compiled inside debug.cpp before debug.cpp includes it. */
bool ParseCommand(char *str);
int CAPTURE_DebugCommand(const char *kind, bool on, std::vector<std::string> &lines);

namespace {

bool CaptureCommand(const char *text) {
    std::string copy(text);
    return ParseCommand(&copy[0]);
}

TEST(DebuggerCapture, ListsEveryKindWithTheFileItWrites) {
    std::vector<std::string> lines;
    EXPECT_EQ(CAPTURE_DebugCommand(NULL, false, lines), 1);
    ASSERT_GE(lines.size(), 3u);
    EXPECT_EQ(lines[0].rfind("DEBUG: CAPTURE WAV off path=", 0), 0u) << lines[0];
    EXPECT_EQ(lines[1].rfind("DEBUG: CAPTURE OPL off path=", 0), 0u) << lines[1];
    EXPECT_EQ(lines[2].rfind("DEBUG: CAPTURE MIDI off path=", 0), 0u) << lines[2];
}

TEST(DebuggerCapture, ANameThatIsNoKindIsNotDoneAndLeavesNoLine) {
    std::vector<std::string> lines;
    EXPECT_EQ(CAPTURE_DebugCommand("TAPE", true, lines), 0);
    EXPECT_TRUE(lines.empty());
}

TEST(DebuggerCapture, TheCommandListsOnItsOwnAndRefusesAKindWithoutAState) {
    EXPECT_TRUE(CaptureCommand("CAPTURE"));
    EXPECT_FALSE(CaptureCommand("CAPTURE WAV")) << "a kind needs ON or OFF";
    EXPECT_FALSE(CaptureCommand("CAPTURE WAV MAYBE"));
    EXPECT_FALSE(CaptureCommand("CAPTURE TAPE ON")) << "no such kind";
    EXPECT_FALSE(CaptureCommand("CAPTURE WAV ON EXTRA")) << "nothing after the state";
}

} // namespace

#endif
