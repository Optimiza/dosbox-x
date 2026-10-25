/*
 *  RUNSTATE (src/debug/debug.cpp): the one line it prints for each state the
 *  debugger can be in.
 */

#include "gtest/gtest.h"

/* debug.h, which declares it, has no include guard, and tests.h is compiled inside debug.cpp
   before debug.cpp includes it. */
const char *DEBUG_RunStateLine(bool debugging, bool debug_running);

namespace {

TEST(DebuggerRunState, StoppedOnlyWhileTheDebuggersLoopHoldsTheMachine) {
    EXPECT_STREQ(DEBUG_RunStateLine(true, false), "DEBUG: RUNSTATE stopped");
    EXPECT_STREQ(DEBUG_RunStateLine(false, false), "DEBUG: RUNSTATE running");
    EXPECT_STREQ(DEBUG_RunStateLine(true, true), "DEBUG: RUNSTATE running")
        << "RUNWATCH runs the guest with the debugger watching";
}

} // namespace
