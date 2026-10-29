/*
 *  RUNSTATE (src/debug/debug.cpp): the one line it prints for each state the
 *  debugger can be in.
 */

#include "gtest/gtest.h"

/* debug.h, which declares it, has no include guard, and tests.h is compiled inside debug.cpp
   before debug.cpp includes it. */
std::string DEBUG_RunStateLine(bool debugging, bool debug_running, unsigned long long cycles,
                               double ms, unsigned long long frames, int fired_bpif);

namespace {

TEST(DebuggerRunState, StoppedOnlyWhileTheDebuggersLoopHoldsTheMachine) {
    EXPECT_EQ(DEBUG_RunStateLine(true, false, 0, 0.0, 0, -1),
              "DEBUG: RUNSTATE stopped cycles=0 ms=0.000000000 frames=0");
    EXPECT_EQ(DEBUG_RunStateLine(false, false, 0, 0.0, 0, -1),
              "DEBUG: RUNSTATE running cycles=0 ms=0.000000000 frames=0");
    EXPECT_EQ(DEBUG_RunStateLine(true, true, 0, 0.0, 0, -1),
              "DEBUG: RUNSTATE running cycles=0 ms=0.000000000 frames=0")
        << "RUNWATCH runs the guest with the debugger watching";
}

TEST(DebuggerRunState, CarriesTheCyclesAndTheEmulatedTimeAtTheirWidest) {
    EXPECT_EQ(DEBUG_RunStateLine(true, false, 18446744073709551615ull, 123456789.123456789,
                                 18446744073709551615ull, -1),
              "DEBUG: RUNSTATE stopped cycles=18446744073709551615 ms=123456789.123456791 "
              "frames=18446744073709551615")
        << "the widest counts and a clock of more than a day, at the window's nine decimals";
}

TEST(DebuggerRunState, NamesTheConditionRowThatStoppedItAndOnlyWhileStopped) {
    EXPECT_EQ(DEBUG_RunStateLine(true, false, 7, 1.5, 3, 0x1A),
              "DEBUG: RUNSTATE stopped cycles=7 ms=1.500000000 frames=3 bpif=1A");
    EXPECT_EQ(DEBUG_RunStateLine(false, false, 7, 1.5, 3, 0x1A),
              "DEBUG: RUNSTATE running cycles=7 ms=1.500000000 frames=3")
        << "a running machine was stopped by nothing";
    EXPECT_EQ(DEBUG_RunStateLine(true, false, 18446744073709551615ull, 123456789.123456789,
                                 18446744073709551615ull, 0xFFFF),
              "DEBUG: RUNSTATE stopped cycles=18446744073709551615 ms=123456789.123456791 "
              "frames=18446744073709551615 bpif=FFFF")
        << "the widest line still fits its buffer";
}

} // namespace
