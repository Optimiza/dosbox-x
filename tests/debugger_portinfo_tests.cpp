/*
 *  PORTINFO (src/debug/debug.cpp): the port it accepts. Which accesses reach a device
 *  depends on the handlers the machine has registered, which a unit test has not set up.
 */

#if C_DEBUG

#include <string>

#include "gtest/gtest.h"

/* debug.h, which declares ParseCommand's neighbours, is not included before tests.h is
   compiled inside debug.cpp. */
bool ParseCommand(char *str);

namespace {

bool PortInfoCommand(const char *text) {
    std::string copy(text);
    return ParseCommand(&copy[0]);
}

TEST(DebuggerPortInfo, TakesOneHexPortWithinTheIoSpace) {
    EXPECT_TRUE(PortInfoCommand("PORTINFO 60"));
    EXPECT_TRUE(PortInfoCommand("PORTINFO 03F8"));
    EXPECT_TRUE(PortInfoCommand("PORTINFO FFFF"));
}

TEST(DebuggerPortInfo, RefusesWhatIsNotOnePort) {
    EXPECT_FALSE(PortInfoCommand("PORTINFO"));
    EXPECT_FALSE(PortInfoCommand("PORTINFO ZZ"));
    EXPECT_FALSE(PortInfoCommand("PORTINFO 10000")) << "one past the 64 KiB of I/O space";
    EXPECT_FALSE(PortInfoCommand("PORTINFO 60 61")) << "one port at a time";
}

} // namespace

#endif
