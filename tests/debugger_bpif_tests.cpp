/*
 *  BPIF (src/debug/debug.cpp): the condition it accepts and the ones it refuses.
 *  A breakpoint is armed when the debugger resumes the machine, which a unit test
 *  does not do, so when it fires is left to a run of the emulator.
 */

#if C_HEAVY_DEBUG

#include "gtest/gtest.h"

/* debug.h, which declares these, has no include guard, and tests.h is compiled inside debug.cpp
   before debug.cpp includes it. */
bool ParseCommand(char *str);

namespace {

bool BpifCommand(const char *text) {
    std::string copy(text);
    return ParseCommand(&copy[0]);
}

class DebuggerBpif : public ::testing::Test {
protected:
    void TearDown() override { BpifCommand("BPDEL *"); }
};

TEST_F(DebuggerBpif, TakesUpToFourTermsOfRegisterOperatorAndHexValue) {
    EXPECT_TRUE(BpifCommand("BPIF AX==3B"));
    EXPECT_TRUE(BpifCommand("BPIF eax>=10 && BX!=0"));
    EXPECT_TRUE(BpifCommand("BPIF AX<1 && BX<=2 && CX>3 && DX>=4"));
    EXPECT_FALSE(BpifCommand("BPIF AX<1 && BX<=2 && CX>3 && DX>=4 && SI==5"))
        << "a fifth term is refused";
}

TEST_F(DebuggerBpif, RefusesWhatItCannotRead) {
    EXPECT_FALSE(BpifCommand("BPIF"));
    EXPECT_FALSE(BpifCommand("BPIF QQ==1")) << "no such register";
    EXPECT_FALSE(BpifCommand("BPIF AX=1")) << "no such comparison";
    EXPECT_FALSE(BpifCommand("BPIF AX~~1")) << "no comparison at all";
    EXPECT_FALSE(BpifCommand("BPIF AX==")) << "no value";
    EXPECT_FALSE(BpifCommand("BPIF AX==1 BX==2")) << "terms are joined by &&";
    EXPECT_FALSE(BpifCommand("BPIF AX==-1")) << "a sign is not part of a hex value";
    EXPECT_FALSE(BpifCommand("BPIF EAX==100000000")) << "wider than 32 bits";
    EXPECT_TRUE(BpifCommand("BPIF EAX==FFFFFFFF")) << "the widest value";
}

} // namespace

#endif // C_HEAVY_DEBUG
