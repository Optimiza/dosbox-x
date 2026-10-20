/*
 *  Presence overlay (src/gui/render_presence.cpp): the command it takes, the
 *  pixel formats it can draw on, and what it does to an output buffer: an edge
 *  on the inner sides only, taken off before the scaler writes and drawn anew
 *  after, so a scaler that rewrites part of a line never gets the overlay
 *  blended onto itself; restored when turned off; and an arrow at the guest's
 *  pointer.
 */

#include <algorithm>
#include <cstring>
#include <vector>

#include "gtest/gtest.h"
#include "render_presence.h"

namespace {

using presence::Format;
using presence::Overlay;
using presence::State;

/* 32-bit pixels as 0x00RRGGBB, as a window surface without alpha has them. */
Format Rgb32() {
    Format f;
    EXPECT_TRUE(presence::FormatFromPixels(4, 0u, 0xFF0000u, 0x00FF00u, 0x0000FFu, f));
    return f;
}

struct Frame {
    unsigned w, h;
    std::vector<uint32_t> px;
    Frame(unsigned width, unsigned height, uint32_t fill) : w(width), h(height), px(width * height, fill) {}
    uint8_t *buf() { return reinterpret_cast<uint8_t *>(px.data()); }
    size_t pitch() const { return w * 4; }
    uint32_t at(unsigned x, unsigned y) const { return px[y * w + x]; }
};

const char *const kLook = " rgb=FF8C00 edge=0.1 steps=2 acting=1 held=0.5 fade=1"
                          " outline=000000 size=0.2 sprite=Xo/oX";

TEST(Presence, TakesAStateAndALookAndRefusesWhatItDoesNotKnow) {
    Overlay o;
    std::string error;
    EXPECT_TRUE(o.Configure(std::string("held") + kLook, error, 0)) << error;
    EXPECT_EQ(o.state(), State::Held);
    EXPECT_FALSE(o.Configure("lit", error, 0));
    EXPECT_FALSE(o.Configure("held colour=FFFFFF", error, 0));
    EXPECT_FALSE(o.Configure("held edge=2", error, 0));
    EXPECT_FALSE(o.Configure("held edge=0x1p-3", error, 0));
    EXPECT_FALSE(o.Configure("held rgb=+12345", error, 0));
    EXPECT_FALSE(o.Configure("held steps=1.5", error, 0));
    EXPECT_FALSE(o.Configure("held sprite=Xo/X", error, 0));
    EXPECT_FALSE(o.Configure("held pointer=yes", error, 0));
    EXPECT_FALSE(o.Configure("", error, 0));
    EXPECT_EQ(o.state(), State::Held) << "a refused command changes nothing";
}

TEST(Presence, ReadsTheFormatsOfTheOutputsTheirOwnBlackIncluded) {
    Format f;
    EXPECT_TRUE(presence::FormatFromPixels(2, 0u, 0xF800u, 0x07E0u, 0x001Fu, f));
    EXPECT_EQ(f.shift[0], 11u);
    EXPECT_EQ(f.max[1], 63u);
    /* OpenGL's pixels carry an opaque alpha byte in every colour, black included */
    EXPECT_TRUE(presence::FormatFromPixels(4, 0xFF000000u, 0xFFFF0000u, 0xFF00FF00u,
                                           0xFF0000FFu, f));
    EXPECT_EQ(f.base, 0xFF000000u);
    EXPECT_EQ(f.mask[0], 0x00FF0000u);
    EXPECT_FALSE(presence::FormatFromPixels(4, 0u, 0xFF0000u, 0xFF0000u, 0xFFu, f));
    EXPECT_FALSE(presence::FormatFromPixels(4, 0u, 0xF0F000u, 0xFF00u, 0xFFu, f));
    EXPECT_FALSE(presence::FormatFromPixels(1, 0u, 0xE0u, 0x1Cu, 0x03u, f));
}

TEST(Presence, DrawsTheEdgeOnlyAndPresentsTheWholeFrameWhenItMoves) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("acting") + kLook, error, 0));
    const Format f = Rgb32();
    o.Resize(20, 20, &f);
    Frame frame(20, 20, 0x00000000u);
    const uint16_t *runs = o.Compose(frame.buf(), frame.pitch(), nullptr, 0, false, 0, 0);
    ASSERT_NE(runs, nullptr);
    EXPECT_EQ(runs[0], 0);
    EXPECT_EQ(runs[1], 20) << "the whole frame is presented when the overlay moves";
    EXPECT_EQ(frame.at(0, 0), 0x00FF8C00u) << "the rim is the colour at full alpha";
    EXPECT_EQ(frame.at(10, 10), 0x00000000u) << "the inside is untouched";
    EXPECT_EQ(frame.at(10, 1), 0x007F4600u) << "the inner band is half way";
    EXPECT_EQ(o.frames(), 1u);
}

TEST(Presence, AScalerThatRewritesPartOfALineNeverGetsTheOverlayBlendedTwice) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("held") + kLook, error, 0));
    const Format f = Rgb32();
    o.Resize(20, 20, &f);
    Frame frame(20, 20, 0x00000000u);
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, false, 0, 0);
    const uint32_t once = frame.at(0, 5);
    for (int n = 0; n < 3; n++) {
        o.Undraw(frame.buf(), frame.pitch());       /* the scaler is about to write */
        EXPECT_EQ(frame.at(0, 5), 0x00000000u) << "the buffer holds the picture alone";
        for (unsigned x = 8; x < 12; x++) frame.px[5 * 20 + x] = 0x00102030u;   /* a cursor */
        const uint16_t changed[] = {5, 1, 14};
        EXPECT_EQ(o.Compose(frame.buf(), frame.pitch(), changed, 0, false, 0, 0), changed)
            << "the scaler's lines are presented when the overlay stayed";
        EXPECT_EQ(frame.at(0, 5), once) << "blended once, however often the line changed";
        EXPECT_EQ(frame.at(10, 5), 0x00102030u);
    }
}

TEST(Presence, AFrameTheScalerNeverOpenedIsRedrawnFromWhatTheOverlayCovered) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("acting") + kLook, error, 0));
    const Format f = Rgb32();
    o.Resize(20, 20, &f);
    Frame frame(20, 20, 0x00445566u);
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, false, 0, 0);
    const uint32_t acting = frame.at(0, 0);
    ASSERT_TRUE(o.Configure("held", error, 0));
    o.Compose(frame.buf(), frame.pitch(), nullptr, 5000, false, 0, 0);   /* no Undraw first */
    EXPECT_NE(frame.at(0, 0), acting);
    ASSERT_TRUE(o.Configure("acting", error, 5000));
    o.Compose(frame.buf(), frame.pitch(), nullptr, 5000, false, 0, 0);
    EXPECT_EQ(frame.at(0, 0), acting) << "drawn over the picture, never over itself";
}

TEST(Presence, TurnedOffItPutsThePictureBackAndThenStaysOutOfTheWay) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("acting") + kLook, error, 0));
    const Format f = Rgb32();
    o.Resize(20, 20, &f);
    Frame frame(20, 20, 0x00123456u);
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, false, 0, 0);
    ASSERT_NE(frame.at(0, 0), 0x00123456u);
    ASSERT_TRUE(o.Configure("off", error, 0));
    EXPECT_TRUE(o.NeedsRepaint(0, false, 0, 0));
    const uint16_t *runs = o.Compose(frame.buf(), frame.pitch(), nullptr, 0, false, 0, 0);
    ASSERT_NE(runs, nullptr);
    EXPECT_EQ(runs[1], 20) << "the frame without it is presented whole";
    EXPECT_EQ(frame.at(0, 0), 0x00123456u);
    EXPECT_EQ(frame.at(19, 19), 0x00123456u);
    const uint16_t changed[] = {0, 20};
    EXPECT_EQ(o.Compose(frame.buf(), frame.pitch(), changed, 0, false, 0, 0), changed);
    EXPECT_FALSE(o.NeedsRepaint(0, false, 0, 0));
}

TEST(Presence, FadesFromActingToHeldOverTheFade) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("acting") + kLook, error, 0));
    const Format f = Rgb32();
    o.Resize(20, 20, &f);
    Frame frame(20, 20, 0x00000000u);
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, false, 0, 0);
    const uint32_t acting = frame.at(0, 0);
    ASSERT_TRUE(o.Configure("held", error, 1000));
    EXPECT_TRUE(o.NeedsRepaint(1250, false, 0, 0));
    o.Compose(frame.buf(), frame.pitch(), nullptr, 1250, false, 0, 0);
    const uint32_t midway = frame.at(0, 0);
    o.Compose(frame.buf(), frame.pitch(), nullptr, 3000, false, 0, 0);
    const uint32_t held = frame.at(0, 0);
    EXPECT_GT(acting >> 16, midway >> 16);
    EXPECT_GT(midway >> 16, held >> 16);
    EXPECT_FALSE(o.NeedsRepaint(4000, false, 0, 0)) << "a finished fade is not repainted";
}

TEST(Presence, ANewLookRepaintsAndLeavesNoTraceOfTheOldOne) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("held") + kLook, error, 0));
    const Format f = Rgb32();
    o.Resize(20, 20, &f);
    Frame frame(20, 20, 0x00000000u);
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, false, 0, 0);
    ASSERT_NE(frame.at(1, 10), 0x00000000u);
    ASSERT_TRUE(o.Configure("held rgb=00FF00 edge=0.05", error, 0));
    EXPECT_TRUE(o.NeedsRepaint(0, false, 0, 0));
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, false, 0, 0);
    EXPECT_EQ(frame.at(1, 10), 0x00000000u) << "the old, wider band is gone";
    EXPECT_EQ(frame.at(0, 10) & 0x00FF00FFu, 0u) << "the new colour, green alone";
    EXPECT_NE(frame.at(0, 10), 0x00000000u);
}

TEST(Presence, DrawsTheArrowAtThePointerOnlyWhenAskedAndMovesIt) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("held") + kLook, error, 0));
    const Format f = Rgb32();
    o.Resize(20, 20, &f);
    Frame frame(20, 20, 0x00FFFFFFu);
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, true, 0.5f, 0.5f);
    EXPECT_EQ(frame.at(10, 10), 0x00FFFFFFu) << "no arrow until the pointer is asked for";
    ASSERT_TRUE(o.Configure("held pointer=1", error, 0));
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, true, 0.5f, 0.5f);
    EXPECT_EQ(frame.at(10, 10), 0x00000000u) << "the arrow's outline at its hot spot";
    EXPECT_EQ(frame.at(12, 10), 0x00FF8C00u) << "its fill beside it";
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, true, 0.25f, 0.25f);
    EXPECT_EQ(frame.at(10, 10), 0x00FFFFFFu) << "the old arrow is gone";
    EXPECT_EQ(frame.at(5, 5), 0x00000000u);
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, true, 0.95f, 0.95f);
    EXPECT_EQ(frame.at(19, 19), 0x00000000u) << "an arrow at the corner is cut, not overrun";
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, false, 0.25f, 0.25f);
    EXPECT_EQ(frame.at(5, 5), 0x00FFFFFFu) << "no driver, no arrow";
}

TEST(Presence, DrawsOnASixteenBitOutputWithAPitchWiderThanItsRows) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("acting") + kLook, error, 0));
    Format f;
    ASSERT_TRUE(presence::FormatFromPixels(2, 0u, 0xF800u, 0x07E0u, 0x001Fu, f));
    o.Resize(20, 20, &f);
    const size_t pitch = 64;                       /* 40 bytes of row, 24 of padding */
    std::vector<uint8_t> buf(pitch * 20, 0x00);
    std::memset(&buf[pitch * 10 + 40], 0xAB, 24);
    o.Compose(buf.data(), pitch, nullptr, 0, false, 0, 0);
    uint16_t rim;
    std::memcpy(&rim, &buf[0], 2);
    EXPECT_EQ(rim, (uint16_t)((31u << 11) | (35u << 5) | 0u)) << "FF8C00 in 5-6-5";
    for (size_t i = pitch * 10 + 40; i < pitch * 11; i++) EXPECT_EQ(buf[i], 0xAB) << i;
}

TEST(Presence, AnOutputItCannotDrawOnIsLeftAlone) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("acting") + kLook, error, 0));
    o.Resize(20, 20, nullptr);
    Frame frame(20, 20, 0x00000000u);
    const uint16_t changed[] = {0, 20};
    EXPECT_EQ(o.Compose(frame.buf(), frame.pitch(), changed, 0, false, 0, 0), changed);
    EXPECT_EQ(frame.at(0, 0), 0x00000000u);
    EXPECT_NE(o.Describe().find("output=none"), std::string::npos);
}

} /* namespace */
