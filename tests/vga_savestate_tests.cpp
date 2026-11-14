/*
 *  VGA_RederiveMemoryHandlers (src/hardware/vga_memory.cpp), which a state load runs: each
 *  complexity flag and the handler at A0000 come out as the register writes left them.
 */

#include <cstdint>
#include <vector>

#include "gtest/gtest.h"

#include "dosbox.h"
#include "inout.h"
#include "mem.h"
#include "paging.h"
#include "vga.h"

bool INT10_SetVideoMode(uint16_t mode);

namespace {

constexpr Bitu A0000_PAGE = 0xA0000 / 4096;

struct Write {
    Bitu port;
    uint8_t index;
    uint8_t value;
};

struct Case {
    const char *name;
    uint16_t mode;
    std::vector<Write> writes;
    unsigned int flag;
    bool raised;
};

class VgaStateLoad : public ::testing::Test {
protected:
    void TearDown() override { INT10_SetVideoMode(0x03); }
};

TEST_F(VgaStateLoad, FlagsAndHandlerFollowTheLoadedRegisters) {
    if (!IS_VGA_ARCH) GTEST_SKIP() << "the complexity flags select a VGA memory handler";
    const std::vector<Case> cases = {
        {"mode 13h", 0x13, {}, 0, false},
        {"map mask", 0x13, {{0x3C4, 2, 0x01}}, VGACMPLX_MAP_MASK, true},
        {"chain 4 overrides the odd/even bit", 0x13, {{0x3C4, 4, 0x0A}}, VGACMPLX_ODDEVEN, false},
        {"non-extended", 0x12, {{0x3C4, 4, 0x04}}, VGACMPLX_NON_EXTENDED, true},
        {"odd/even", 0x12, {{0x3C4, 4, 0x02}}, VGACMPLX_ODDEVEN, true},
        {"set/reset", 0x12, {{0x3CE, 1, 0x0F}}, VGACMPLX_SETRESET, true},
        {"raster op", 0x12, {{0x3CE, 3, 0x18}}, VGACMPLX_ROPROT, true},
        {"write mode", 0x12, {{0x3CE, 5, 0x02}}, VGACMPLX_WRITEMODE, true},
        {"read mode", 0x12, {{0x3CE, 5, 0x08}}, VGACMPLX_READMODE, true},
        {"color don't care", 0x12, {{0x3CE, 7, 0x0F}, {0x3CE, 5, 0x08}}, VGACMPLX_COLORDONTCARE, true},
        {"bit mask", 0x12, {{0x3CE, 8, 0x0F}}, VGACMPLX_BITMASK, true},
    };
    for (const Case &c : cases) {
        SCOPED_TRACE(c.name);
        ASSERT_TRUE(INT10_SetVideoMode(c.mode));
        for (const Write &w : c.writes) {
            IO_WriteB(w.port, w.index);
            IO_WriteB(w.port + 1, w.value);
        }
        const unsigned int flags = vga.complexity.flags;
        ASSERT_EQ((flags & c.flag) != 0, c.raised) << "the register write itself";
        const VGA_Seq seq = vga.seq;
        const VGA_Gfx gfx = vga.gfx;
        const VGA_Config config = vga.config;
        const VGAModes mode = vga.mode;
        PageHandler *const handler = MEM_GetPageHandler(A0000_PAGE);

        ASSERT_TRUE(INT10_SetVideoMode(0x03));
        vga.seq = seq;
        vga.gfx = gfx;
        vga.config = config;
        vga.mode = mode;
        VGA_RederiveMemoryHandlers();

        EXPECT_EQ(vga.complexity.flags, flags);
        EXPECT_EQ(MEM_GetPageHandler(A0000_PAGE), handler);
    }
}

} // namespace
