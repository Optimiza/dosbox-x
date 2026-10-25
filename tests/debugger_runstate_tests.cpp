/*
 *  RUNSTATE (src/debug/debug.cpp): the one line it prints for each state the
 *  debugger can be in.
 */

#include "gtest/gtest.h"

/* debug.h, which declares it, has no include guard, and tests.h is compiled inside debug.cpp
   before debug.cpp includes it. */
std::string DEBUG_RunStateLine(bool debugging, bool debug_running, unsigned long long cycles,
                               double ms);

namespace {

TEST(DebuggerRunState, StoppedOnlyWhileTheDebuggersLoopHoldsTheMachine) {
    EXPECT_EQ(DEBUG_RunStateLine(true, false, 0, 0.0),
              "DEBUG: RUNSTATE stopped cycles=0 ms=0.000000000");
    EXPECT_EQ(DEBUG_RunStateLine(false, false, 0, 0.0),
              "DEBUG: RUNSTATE running cycles=0 ms=0.000000000");
    EXPECT_EQ(DEBUG_RunStateLine(true, true, 0, 0.0),
              "DEBUG: RUNSTATE running cycles=0 ms=0.000000000")
        << "RUNWATCH runs the guest with the debugger watching";
}

TEST(DebuggerRunState, CarriesTheCyclesAndTheEmulatedTimeAtTheirWidest) {
    EXPECT_EQ(DEBUG_RunStateLine(true, false, 18446744073709551615ull, 123456789.123456789),
              "DEBUG: RUNSTATE stopped cycles=18446744073709551615 ms=123456789.123456791")
        << "the widest count and a clock of more than a day, at the window's nine decimals";
}

} // namespace
