/*
 *  TAP (src/debug/debug.cpp): the byte enables an access is told in, and the file name
 *  taken back as it was typed. Installing a tap needs a page of guest RAM, which a unit
 *  test has not got.
 */

#if C_HEAVY_DEBUG

#include <cstdint>
#include <string>

#include "gtest/gtest.h"

/* debug.h is not included before tests.h is compiled inside debug.cpp. */
void DEBUG_TapLanes(uint32_t at, unsigned int width, uint32_t value, uint32_t dword, uint32_t &data, uint32_t &mask);
std::string DEBUG_TapPathFromTyped(const std::string &raw, const char *upper_tail);

namespace {

struct Lanes {
    uint32_t data, mask;
};

Lanes TapLanesOf(uint32_t at, unsigned int width, uint32_t value, uint32_t dword) {
    Lanes l;
    DEBUG_TapLanes(at, width, value, dword, l.data, l.mask);
    return l;
}

TEST(DebuggerTap, ADwordAccessDrivesAllFourLanes) {
    const Lanes l = TapLanesOf(0x1000, 4, 0x11223344u, 0x1000);
    EXPECT_EQ(l.data, 0x11223344u);
    EXPECT_EQ(l.mask, 0xFFFFFFFFu);
}

TEST(DebuggerTap, AByteDrivesTheLaneItsAddressNames) {
    const Lanes l = TapLanesOf(0x1002, 1, 0xAB, 0x1000);
    EXPECT_EQ(l.data, 0x00AB0000u);
    EXPECT_EQ(l.mask, 0x00FF0000u);
}

TEST(DebuggerTap, AWordThatCrossesADwordIsSplitBetweenTheTwo) {
    const Lanes first = TapLanesOf(0x1003, 2, 0xBBAA, 0x1000);
    EXPECT_EQ(first.data, 0xAA000000u) << "the low byte lands in the last lane of the first dword";
    EXPECT_EQ(first.mask, 0xFF000000u);
    const Lanes second = TapLanesOf(0x1003, 2, 0xBBAA, 0x1004);
    EXPECT_EQ(second.data, 0x000000BBu) << "the high byte lands in the first lane of the next";
    EXPECT_EQ(second.mask, 0x000000FFu);
}

TEST(DebuggerTap, AnAccessOutsideTheDwordDrivesNoLane) {
    const Lanes l = TapLanesOf(0x1008, 4, 0xFFFFFFFFu, 0x1000);
    EXPECT_EQ(l.data, 0u);
    EXPECT_EQ(l.mask, 0u);
}

TEST(DebuggerTap, TheFileNameComesBackAsTypedCountedFromTheEnd) {
    EXPECT_EQ(DEBUG_TapPathFromTyped("W 0:10 /Tmp/File.Txt", "/TMP/FILE.TXT"), "/Tmp/File.Txt");
    EXPECT_EQ(DEBUG_TapPathFromTyped("W 0:10 a b.Txt", "A B.TXT"), "a b.Txt") << "a space inside the name";
}

TEST(DebuggerTap, ATailLongerThanTheLineIsLeftAsItIs) {
    EXPECT_EQ(DEBUG_TapPathFromTyped("F", "FILE"), "FILE");
}

} // namespace

#endif
