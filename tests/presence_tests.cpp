/*
 *  Presence overlay (src/gui/render_presence.cpp): the command it takes, the
 *  pixel formats it can draw on, and what it does to an output buffer: an edge
 *  on the inner sides only, taken off before the scaler writes and drawn anew
 *  after, so a scaler that rewrites part of a line never gets the overlay
 *  blended onto itself; restored when turned off; and an arrow at the guest's
 *  pointer; the layers of protocol 3 (keycaps, caption, pause mark) and where `auto` puts
 *  them. The cost cases print what each case costs and hold it at its floor.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "render_presence.h"

extern uint8_t int10_font_16[256 * 16];

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
                          " outline=000000 sprite=Xo/oX";

TEST(Presence, TakesAStateAndALookAndRefusesWhatItDoesNotKnow) {
    Overlay o;
    std::string error;
    EXPECT_TRUE(o.Configure(std::string("held") + kLook, error, 0)) << error;
    EXPECT_EQ(o.state(), State::Held);
    EXPECT_FALSE(o.Configure("lit", error, 0));
    EXPECT_FALSE(o.Configure("held colour=FFFFFF", error, 0));
    EXPECT_FALSE(o.Configure("held edge=2", error, 0));
    EXPECT_FALSE(o.Configure("held edge=0x1p-3", error, 0));
    EXPECT_TRUE(o.Configure("held edge=0.001", error, 0)) << "a bound is a value it takes";
    EXPECT_FALSE(o.Configure("held edge=0.0009", error, 0));
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

TEST(Presence, TheBareCommandAnnouncesItsVersion) {
    std::vector<std::string> lines;
    EXPECT_TRUE(PRESENCE_Command("", lines));
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find(" version=3"), std::string::npos) << lines[0];
    EXPECT_TRUE(PRESENCE_Command("  ", lines));
    EXPECT_NE(lines[0].find(" version=3"), std::string::npos) << lines[0];
    EXPECT_EQ(presence::Overlay().Describe().find("version="), std::string::npos)
        << "only the bare command announces it";
}

TEST(Presence, DrawsTheEdgeOnlyAndPresentsTheWholeFrameWhenItMoves) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("acting") + kLook, error, 0));
    const Format f = Rgb32();
    o.Resize(20, 20, &f);
    o.Guest(10, 10);
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
    o.Guest(10, 10);
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
    o.Guest(10, 10);
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
    o.Guest(10, 10);
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
    o.Guest(10, 10);
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
    o.Guest(10, 10);
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
    o.Guest(10, 10);
    Frame frame(20, 20, 0x00FFFFFFu);
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, true, 0.5f, 0.5f);
    EXPECT_EQ(frame.at(10, 10), 0x00FFFFFFu) << "no arrow until the pointer is asked for";
    ASSERT_TRUE(o.Configure("held pointer=1 glide=0", error, 0));     /* no glide: it jumps */
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

TEST(Presence, DrawsTheArrowInTheGuestsPixelsAsTheOutputScalesThem) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("held") + kLook + " pointer=1 glide=0", error, 0)) << error;
    EXPECT_FALSE(o.Configure("held size=0.2", error, 0)) << "the guest's pixels size it, not a share";
    const Format f = Rgb32();
    o.Resize(40, 30, &f);
    o.Guest(20, 10);                  /* each guest pixel 2 wide and 3 tall */
    Frame frame(40, 30, 0x00FFFFFFu);
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, true, 0.5f, 0.5f);
    EXPECT_EQ(frame.at(21, 17), 0x00000000u) << "the hot spot's cell, 2 x 3";
    EXPECT_EQ(frame.at(22, 15), 0x00FF8C00u) << "the fill starts one guest pixel right";
    EXPECT_EQ(frame.at(20, 18), 0x00FF8C00u) << "and one guest pixel down";
    EXPECT_EQ(frame.at(24, 15), 0x00FFFFFFu) << "two guest pixels wide, no more";
    o.Resize(30, 25, &f);
    o.Guest(20, 10);                  /* 1.5 by 2.5: the cells take turns */
    Frame odd(30, 25, 0x00FFFFFFu);
    o.Compose(odd.buf(), odd.pitch(), nullptr, 0, true, 0.5f, 0.5f);   /* hot spot at 15,13 */
    EXPECT_EQ(odd.at(16, 15), 0x00000000u);
    EXPECT_EQ(odd.at(17, 13), 0x00FF8C00u) << "the first cell is 2 wide";
    EXPECT_EQ(odd.at(15, 16), 0x00FF8C00u) << "and 3 tall";
    EXPECT_EQ(odd.at(17, 17), 0x00000000u);
    EXPECT_EQ(odd.at(18, 17), 0x00FFFFFFu) << "the two cells are 3 wide together";
    EXPECT_EQ(odd.at(15, 18), 0x00FFFFFFu) << "and 5 tall together";
}

TEST(Presence, TheHotSpotSitsOnTheWholeGuestPixelThePointerIsIn) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("held") + kLook + " pointer=1 glide=0", error, 0)) << error;
    const Format f = Rgb32();
    o.Resize(40, 30, &f);
    o.Guest(20, 10);                  /* 2 by 3 output pixels a guest pixel */
    Frame frame(40, 30, 0x00FFFFFFu);
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, true, 0.275f, 0.5f);  /* guest x 5.5 */
    EXPECT_EQ(frame.at(10, 15), 0x00000000u) << "guest pixel 5 starts at output x 10, not 11";
    EXPECT_EQ(frame.at(9, 15), 0x00FFFFFFu);
}

TEST(Presence, AnArrowScaledToNothingLeavesNoEmptySpanToUndraw) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("held") + kLook + " pointer=1 glide=0", error, 0)) << error;
    const Format f = Rgb32();
    o.Resize(40, 30, &f);
    o.Guest(1000, 1000);              /* 0.04 output pixels a guest pixel: every span rounds to 0 */
    Frame frame(40, 30, 0x00FFFFFFu);
    const std::vector<uint32_t> clean = frame.px;
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, true, 0.5f, 0.5f);
    o.Undraw(frame.buf(), frame.pitch());
    EXPECT_EQ(frame.px, clean);
}

TEST(Presence, TheArrowGlidesToItsNewPlaceEasingOut) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure("held rgb=FF8C00 edge=0.01 steps=1 acting=1 held=0.5 fade=1"
                            " outline=000000 sprite=Xo/oX pointer=1 glide=0.2", error, 0))
        << error;
    const Format f = Rgb32();
    o.Resize(100, 100, &f);
    o.Guest(10, 10);
    Frame frame(100, 100, 0x00FFFFFFu);
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, true, 0.2f, 0.2f);
    EXPECT_EQ(frame.at(20, 20), 0x00000000u) << "the first arrow appears where it is";
    o.Compose(frame.buf(), frame.pitch(), nullptr, 1000, true, 0.8f, 0.8f);
    EXPECT_EQ(frame.at(20, 20), 0x00000000u) << "a new place is glided to from the old one";
    EXPECT_TRUE(o.NeedsRepaint(1100, true, 0.8f, 0.8f)) << "a gliding arrow repaints the window";
    o.Compose(frame.buf(), frame.pitch(), nullptr, 1100, true, 0.8f, 0.8f);
    EXPECT_EQ(frame.at(20, 20), 0x00FFFFFFu) << "half way it has left";
    EXPECT_EQ(frame.at(95, 95), 0x00FFFFFFu) << "half way it has not arrived";
    o.Compose(frame.buf(), frame.pitch(), nullptr, 1200, true, 0.8f, 0.8f);
    EXPECT_EQ(frame.at(95, 95), 0x00000000u) << "at the end it is there";
    EXPECT_FALSE(o.NeedsRepaint(1300, true, 0.8f, 0.8f)) << "and nothing more is repainted";
}

TEST(Presence, DrawsOnASixteenBitOutputWithAPitchWiderThanItsRows) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("acting") + kLook, error, 0));
    Format f;
    ASSERT_TRUE(presence::FormatFromPixels(2, 0u, 0xF800u, 0x07E0u, 0x001Fu, f));
    o.Resize(20, 20, &f);
    o.Guest(10, 10);
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
    o.Guest(10, 10);
    Frame frame(20, 20, 0x00000000u);
    const uint16_t changed[] = {0, 20};
    EXPECT_EQ(o.Compose(frame.buf(), frame.pitch(), changed, 0, false, 0, 0), changed);
    EXPECT_EQ(frame.at(0, 0), 0x00000000u);
    EXPECT_NE(o.Describe().find("output=undrawable"), std::string::npos)
        << "an output that is there but cannot be drawn on is not one still to come";
    Overlay fresh;
    EXPECT_NE(fresh.Describe().find("output=none"), std::string::npos);
}

TEST(Presence, AFrameDrawnButNeverShownIsDrawnAgain) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("acting") + kLook, error, 0));
    const Format f = Rgb32();
    o.Resize(20, 20, &f);
    o.Guest(10, 10);
    Frame frame(20, 20, 0x00000000u);
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, false, 0, 0, false);
    EXPECT_TRUE(o.NeedsRepaint(0, false, 0, 0)) << "an aborted frame showed nothing";
    EXPECT_EQ(o.cost().presented, 0u) << "and presented nothing";
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, false, 0, 0);
    EXPECT_FALSE(o.NeedsRepaint(0, false, 0, 0));
}

TEST(Presence, TheSameLookSentAgainIsNotANewOne) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("acting") + kLook, error, 0));
    const Format f = Rgb32();
    o.Resize(20, 20, &f);
    o.Guest(10, 10);
    Frame frame(20, 20, 0x00000000u);
    const uint16_t changed[] = {0, 20};
    o.Compose(frame.buf(), frame.pitch(), changed, 0, false, 0, 0);
    ASSERT_TRUE(o.Configure(std::string("acting") + kLook, error, 0));
    EXPECT_FALSE(o.NeedsRepaint(0, false, 0, 0)) << "an unchanged look repaints the whole frame";
    ASSERT_TRUE(o.Configure(std::string("acting") + kLook + " rgb=00FF00", error, 0));
    EXPECT_TRUE(o.NeedsRepaint(0, false, 0, 0));
}

/* A 320x200 output of a 320x200 guest, one output pixel a guest pixel, so the band's layout
   reads in guest pixels: the edge is 5 wide, the band is the bottom 25 rows, its boxes are 20
   rows from row 177, inset 7 from each side. */
struct Band {
    Overlay o;
    Frame frame;
    explicit Band(const std::string &words, uint32_t picture = 0x00000000u) : frame(320, 200, picture) {
        std::string error;
        EXPECT_TRUE(o.Configure(words + " edge=0.025 caption_in=0 caption_out=0", error, 0)) << error;
        const Format f = Rgb32();
        o.Resize(320, 200, &f);
        o.Guest(320, 200);
    }
    void Draw(uint32_t now_ms) { o.Compose(frame.buf(), frame.pitch(), nullptr, now_ms, false, 0, 0); }
};
const unsigned kBoxY = 177, kBoxH = 20, kInset = 7;

/* The first lit pixel of a glyph in the VGA 8x16 font, as (column, row). */
void FirstInk(unsigned char c, unsigned &col, unsigned &row) {
    for (row = 0; row < 16; row++)
        for (col = 0; col < 8; col++)
            if (int10_font_16[c * 16 + row] & (0x80u >> col)) return;
}

TEST(PresenceLayers, AKeycapIsABoxPerKeyWithTheKeysGlyphs) {
    Band b("held keys=A,Shift place=bottom");
    b.Draw(0);
    /* laid from the right inset: "Shift" is 5 x 8 + 4 wide, "A" 12 and a gap before it */
    const unsigned shift_x0 = 320 - kInset - 44, a_x0 = shift_x0 - 2 - 12;
    unsigned col = 0, row = 0;
    FirstInk('A', col, row);
    EXPECT_EQ(b.frame.at(a_x0 + 2 + col, kBoxY + 2 + row), 0x00FFFFFFu) << "the glyph's ink";
    const uint32_t cap = b.frame.at(a_x0, kBoxY);
    EXPECT_NE(cap, 0u) << "the keycap's box";
    EXPECT_EQ(cap & 0xFFu, (cap >> 16) & 0xFFu) << "grey, as keycap_rgb is";
    EXPECT_EQ(b.frame.at(a_x0 - 1, kBoxY), 0u) << "nothing between the boxes";
    EXPECT_EQ(b.frame.at(a_x0, kBoxY - 1), 0u) << "nothing above them";
    std::string error;
    ASSERT_TRUE(b.o.Configure("held keys=", error, 0));
    b.Draw(0);
    EXPECT_EQ(b.frame.at(a_x0 + 2 + col, kBoxY + 2 + row), 0u) << "no key down, no keycap";
}

TEST(PresenceLayers, ThePauseMarkIsDrawnOnlyWhilePaused) {
    Band b("paused place=bottom");
    b.Draw(0);
    const unsigned x0 = kInset;
    EXPECT_EQ(b.frame.at(x0 + 2, kBoxY + 2), 0x00FFFFFFu) << "the first bar";
    EXPECT_EQ(b.frame.at(x0 + 8, kBoxY + 2), 0x00FFFFFFu) << "the second bar";
    EXPECT_NE(b.frame.at(x0 + 5, kBoxY + 2), 0x00FFFFFFu) << "the gap between them";
    EXPECT_NE(b.frame.at(x0 + 5, kBoxY + 2), 0u);
    std::string error;
    ASSERT_TRUE(b.o.Configure("held", error, 0));
    b.Draw(0);
    EXPECT_EQ(b.frame.at(x0 + 2, kBoxY + 2), 0u) << "held is not paused";
}

TEST(PresenceLayers, ACaptionThatFitsIsCentredInItsTint) {
    Band b("held caption=hi%20there tint=5AC8FA serial=1 place=bottom");
    b.Draw(0);
    /* "hi there" is 8 x 8 + 4 wide, centred in the 306 between the insets */
    const unsigned x0 = kInset + (320 - 2 * kInset - 68) / 2;
    const uint32_t box = b.frame.at(x0, kBoxY);
    EXPECT_GT(box & 0xFFu, (box >> 16) & 0xFFu) << "the read tint, more blue than red";
    EXPECT_EQ(b.frame.at(x0 - 1, kBoxY), 0u) << "the box ends where the text does";
    EXPECT_EQ(b.frame.at(x0 + 68, kBoxY), 0u);
    unsigned col = 0, row = 0;
    FirstInk('h', col, row);
    EXPECT_EQ(b.frame.at(x0 + 2 + col, kBoxY + 2 + row), 0x00FFFFFFu) << "the text, %20 a space";
    FirstInk('t', col, row);
    EXPECT_EQ(b.frame.at(x0 + 2 + 3 * 8 + col, kBoxY + 2 + row), 0x00FFFFFFu);
    EXPECT_FALSE(b.o.NeedsRepaint(5000, false, 0, 0)) << "a caption that fits stays still";
}

TEST(PresenceLayers, ACaptionTooWideForItsRoomScrollsAGuestPixelAtATime) {
    const std::string text(60, 'W');
    Band b("held scroll_cps=12 serial=1 place=bottom caption=" + text);
    b.Draw(0);
    std::vector<uint32_t> before(b.frame.px.begin() + (kBoxY + 8) * 320,
                                 b.frame.px.begin() + (kBoxY + 9) * 320);
    /* 12 characters a second is 96 guest pixels: one every 10.4 ms */
    EXPECT_FALSE(b.o.NeedsRepaint(10, false, 0, 0)) << "less than a pixel is not a step";
    EXPECT_TRUE(b.o.NeedsRepaint(11, false, 0, 0));
    b.Draw(11);
    for (unsigned x = kInset + 2; x + 1 < 320 - kInset - 2; x++)
        ASSERT_EQ(b.frame.at(x, kBoxY + 8), before[x + 1]) << "moved one pixel left at " << x;
}

TEST(PresenceLayers, ACaptionLeavesWithItsFadeAndTheSameSerialRestartsNothing) {
    Band b("held caption=go serial=7 place=bottom");
    std::string error;
    ASSERT_TRUE(b.o.Configure("held caption_in=0.2 caption_out=0.1", error, 0));
    b.Draw(1000);
    const unsigned x0 = kInset + (320 - 2 * kInset - 20) / 2;
    const uint32_t shown = b.frame.at(x0, kBoxY);
    ASSERT_NE(shown, 0u);
    ASSERT_TRUE(b.o.Configure("held caption=go serial=7", error, 1000));
    EXPECT_FALSE(b.o.NeedsRepaint(1000, false, 0, 0)) << "the writer's retry is not a new caption";
    ASSERT_TRUE(b.o.Configure("held caption=go serial=8", error, 1000));
    EXPECT_TRUE(b.o.NeedsRepaint(1000, false, 0, 0)) << "a new serial is";
    b.Draw(1000);
    EXPECT_EQ(b.frame.at(x0, kBoxY), 0u) << "and it fades in from nothing";
    b.Draw(1100);
    EXPECT_LT(b.frame.at(x0, kBoxY) >> 16, shown >> 16) << "half way in";
    b.Draw(1200);
    EXPECT_EQ(b.frame.at(x0, kBoxY), shown);
    ASSERT_TRUE(b.o.Configure("held caption=", error, 2000));
    b.Draw(2050);
    EXPECT_LT(b.frame.at(x0, kBoxY) >> 16, shown >> 16) << "leaving";
    b.Draw(2100);
    EXPECT_EQ(b.frame.at(x0, kBoxY), 0u) << "gone";
    EXPECT_FALSE(b.o.NeedsRepaint(3000, false, 0, 0));
}

TEST(PresenceLayers, TheLayersWordTurnsLayersOffAndPlaceMovesTheBand) {
    Band b("paused keys=A layers=edge,keys place=top");
    b.Draw(0);
    EXPECT_EQ(b.frame.at(kInset + 2, kBoxY + 2), 0u) << "no pause layer asked";
    const unsigned a_x0 = 320 - kInset - 12;
    /* the top band's boxes: 25 rows, the box in their middle */
    EXPECT_NE(b.frame.at(a_x0, 2), 0u) << "the keycap at the top";
    EXPECT_EQ(b.frame.at(a_x0, kBoxY), 0u) << "and not at the bottom";
    EXPECT_NE(b.o.Describe().find(" layers=edge,keys place=top"), std::string::npos) << b.o.Describe();
}

TEST(PresenceLayers, AnOutputItCannotDrawOnSaysWhichLayersItLacks) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure("held layers=edge,calls", error, 0));
    o.Resize(20, 20, nullptr);
    EXPECT_NE(o.Describe().find(" output=undrawable layers= place=bottom lacks=edge,calls"),
              std::string::npos) << o.Describe();
}

TEST(PresenceLayers, TheProtocolRefusesWhatItCannotDrawAndChangesNothing) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure("held caption=a serial=1 keys=Esc", error, 0)) << error;
    EXPECT_FALSE(o.Configure("held layers=edge,captions", error, 0)) << "an unknown layer";
    EXPECT_FALSE(o.Configure("held caption=%2", error, 0)) << "a cut escape";
    EXPECT_FALSE(o.Configure("held caption=%G0", error, 0));
    EXPECT_FALSE(o.Configure("held caption=" + std::string(161, 'x'), error, 0)) << "over 160 bytes";
    EXPECT_TRUE(o.Configure("held caption=" + std::string(160, 'x') + " serial=2", error, 0));
    EXPECT_FALSE(o.Configure("held keys=a,b,c,d,e,f,g,h,i", error, 0)) << "over 8 keys";
    EXPECT_FALSE(o.Configure("held keys=ThirteenBytes", error, 0)) << "a key over 12 bytes";
    EXPECT_FALSE(o.Configure("held keys=a,,b", error, 0)) << "an empty key";
    EXPECT_FALSE(o.Configure("held place=left", error, 0));
    EXPECT_FALSE(o.Configure("held tint=red", error, 0));
    EXPECT_FALSE(o.Configure("held serial=-1", error, 0));
    EXPECT_FALSE(o.Configure("held caption_alpha=2", error, 0));
    EXPECT_FALSE(o.Configure("paused place=top caption=%ZZ", error, 0));
    EXPECT_EQ(o.state(), State::Held) << "a refused command changes nothing";
    EXPECT_TRUE(o.Configure("paused layers= keys=%2C", error, 0)) << "no layer, a comma key";
}

/* scry_core's src/scry_core/placement_vectors.txt, verbatim: the three renderers replay it. */
const char *const kPlacementVectors = R"(# The placement rules of scry_core.placement, for a renderer in any language to replay.
# Words are separated by one space; colours are 6 hex digits; points are x,y.
# points <width> <height> top <x,y>... bottom <x,y>...
# score <colour>... = <count>
# walk <start band> <top>/<bottom>... = <band after each sample>...
constants band_divisor=8 columns=16 rows=2 margin=4 consecutive=2 start=bottom sample_s=1
points 320 200 top 10,6 30,6 50,6 70,6 90,6 110,6 130,6 150,6 170,6 190,6 210,6 230,6 250,6 270,6 290,6 310,6 10,18 30,18 50,18 70,18 90,18 110,18 130,18 150,18 170,18 190,18 210,18 230,18 250,18 270,18 290,18 310,18 bottom 10,181 30,181 50,181 70,181 90,181 110,181 130,181 150,181 170,181 190,181 210,181 230,181 250,181 270,181 290,181 310,181 10,193 30,193 50,193 70,193 90,193 110,193 130,193 150,193 170,193 190,193 210,193 230,193 250,193 270,193 290,193 310,193
points 640 480 top 20,15 60,15 100,15 140,15 180,15 220,15 260,15 300,15 340,15 380,15 420,15 460,15 500,15 540,15 580,15 620,15 20,45 60,45 100,45 140,45 180,45 220,45 260,45 300,45 340,45 380,45 420,45 460,45 500,45 540,45 580,45 620,45 bottom 20,435 60,435 100,435 140,435 180,435 220,435 260,435 300,435 340,435 380,435 420,435 460,435 500,435 540,435 580,435 620,435 20,465 60,465 100,465 140,465 180,465 220,465 260,465 300,465 340,465 380,465 420,465 460,465 500,465 540,465 580,465 620,465
points 256 224 top 8,7 24,7 40,7 56,7 72,7 88,7 104,7 120,7 136,7 152,7 168,7 184,7 200,7 216,7 232,7 248,7 8,21 24,21 40,21 56,21 72,21 88,21 104,21 120,21 136,21 152,21 168,21 184,21 200,21 216,21 232,21 248,21 bottom 8,203 24,203 40,203 56,203 72,203 88,203 104,203 120,203 136,203 152,203 168,203 184,203 200,203 216,203 232,203 248,203 8,217 24,217 40,217 56,217 72,217 88,217 104,217 120,217 136,217 152,217 168,217 184,217 200,217 216,217 232,217 248,217
points 384 224 top 12,7 36,7 60,7 84,7 108,7 132,7 156,7 180,7 204,7 228,7 252,7 276,7 300,7 324,7 348,7 372,7 12,21 36,21 60,21 84,21 108,21 132,21 156,21 180,21 204,21 228,21 252,21 276,21 300,21 324,21 348,21 372,21 bottom 12,203 36,203 60,203 84,203 108,203 132,203 156,203 180,203 204,203 228,203 252,203 276,203 300,203 324,203 348,203 372,203 12,217 36,217 60,217 84,217 108,217 132,217 156,217 180,217 204,217 228,217 252,217 276,217 300,217 324,217 348,217 372,217
points 16 8 top 0,0 1,0 2,0 3,0 4,0 5,0 6,0 7,0 8,0 9,0 10,0 11,0 12,0 13,0 14,0 15,0 0,0 1,0 2,0 3,0 4,0 5,0 6,0 7,0 8,0 9,0 10,0 11,0 12,0 13,0 14,0 15,0 bottom 0,7 1,7 2,7 3,7 4,7 5,7 6,7 7,7 8,7 9,7 10,7 11,7 12,7 13,7 14,7 15,7 0,7 1,7 2,7 3,7 4,7 5,7 6,7 7,7 8,7 9,7 10,7 11,7 12,7 13,7 14,7 15,7
points 1 1 top 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 bottom 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0 0,0
score 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 = 0
score 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 000000 FFFFFF FF0000 = 2
score 101010 202020 101010 202020 101010 202020 101010 202020 101010 202020 101010 202020 101010 202020 101010 202020 101010 202020 101010 202020 101010 202020 101010 202020 101010 202020 101010 202020 101010 202020 101010 202020 = 16
score 000000 000001 000002 000003 000004 000005 000006 000007 000008 000009 00000A 00000B 00000C 00000D 00000E 00000F 000010 000011 000012 000013 000014 000015 000016 000017 000018 000019 00001A 00001B 00001C 00001D 00001E 00001F = 31
score 00AA00 00AA00 00AA00 00AA00 00AA00 00AA00 00AA00 00AA00 00AA00 00AA00 00AA00 00AA00 00AA00 00AA00 00AA00 00AA00 00AA00 00AA00 00AA00 00AA00 0000AA 0000AA 0000AA 0000AA 0000AA 0000AA 0000AA 0000AA 0000AA 0000AA 0000AA 0000AA = 12
walk bottom 0/10 0/10 0/10 = bottom top top
walk bottom 0/10 9/10 0/10 0/10 = bottom bottom bottom top
walk bottom 6/10 7/10 6/10 = bottom bottom bottom
walk bottom 7/10 6/10 = bottom bottom
walk top 20/0 20/0 0/20 0/20 = top bottom bottom top
walk top 0/0 32/0 0/0 32/0 = top top top top
)";

std::vector<std::string> Words(const std::string &line) {
    std::vector<std::string> words;
    std::istringstream in(line);
    std::string w;
    while (in >> w) words.push_back(w);
    return words;
}

presence::placement::Point PointWord(const std::string &w) {
    const size_t comma = w.find(',');
    return {(unsigned)std::stoul(w.substr(0, comma)), (unsigned)std::stoul(w.substr(comma + 1))};
}

TEST(PresencePlacement, ReplaysScryCoresVectors) {
    namespace pl = presence::placement;
    std::istringstream in(kPlacementVectors);
    std::string line;
    unsigned checked = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> w = Words(line);
        if (w[0] == "constants") {
            EXPECT_EQ(line, "constants band_divisor=" + std::to_string(pl::BAND_DIVISOR) +
                            " columns=" + std::to_string(pl::COLUMNS) + " rows=" + std::to_string(pl::ROWS) +
                            " margin=" + std::to_string(pl::MARGIN) + " consecutive=" +
                            std::to_string(pl::CONSECUTIVE) + " start=bottom sample_s=" +
                            std::to_string(pl::SAMPLE_MS / 1000));
            EXPECT_EQ(Overlay().place(), presence::Place::Bottom) << "the start band";
        } else if (w[0] == "points") {
            pl::Point top[pl::SAMPLES], bottom[pl::SAMPLES];
            pl::Points((unsigned)std::stoul(w[1]), (unsigned)std::stoul(w[2]), top, bottom);
            ASSERT_EQ(w.size(), 5u + 2 * pl::SAMPLES) << line;
            for (unsigned i = 0; i < pl::SAMPLES; i++) {
                const pl::Point t = PointWord(w[4 + i]), b = PointWord(w[5 + pl::SAMPLES + i]);
                EXPECT_EQ(top[i].x, t.x) << line;
                EXPECT_EQ(top[i].y, t.y) << line;
                EXPECT_EQ(bottom[i].x, b.x) << line;
                EXPECT_EQ(bottom[i].y, b.y) << line;
            }
        } else if (w[0] == "score") {
            ASSERT_EQ(w.size(), 3u + pl::SAMPLES) << line;
            uint32_t colours[pl::SAMPLES];
            for (unsigned i = 0; i < pl::SAMPLES; i++) colours[i] = (uint32_t)std::stoul(w[1 + i], nullptr, 16);
            EXPECT_EQ(pl::Score(colours), (unsigned)std::stoul(w.back())) << line;
        } else if (w[0] == "walk") {
            presence::Place band = w[1] == "top" ? presence::Place::Top : presence::Place::Bottom;
            unsigned streak = 0;
            const size_t eq = (size_t)(std::find(w.begin(), w.end(), "=") - w.begin());
            ASSERT_EQ(w.size() - eq - 1, eq - 2) << line;
            for (size_t i = 2; i < eq; i++) {
                const size_t slash = w[i].find('/');
                band = pl::Step(band, streak, (unsigned)std::stoul(w[i].substr(0, slash)),
                                (unsigned)std::stoul(w[i].substr(slash + 1)));
                EXPECT_EQ(band == presence::Place::Top ? "top" : "bottom", w[eq + 1 + i - 2]) << line << " at " << i;
            }
        } else {
            ADD_FAILURE() << "a vector this replay does not know: " << line;
        }
        checked++;
    }
    EXPECT_GT(checked, 10u) << "the vectors were read";
}

/* A picture whose bottom band is busy and whose top band is one colour. */
uint32_t BusyBottom(const void *, unsigned x, unsigned y) { return y >= 175 ? x * 0x010101u : 0x123456u; }

TEST(PresencePlacement, AutoMovesTheBandOnceTheOtherShowsLessOnTwoSamples) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure("held place=auto", error, 0));
    const Format f = Rgb32();
    o.Resize(32, 20, &f);
    presence::Picture picture;
    picture.width = 320;
    picture.height = 200;
    picture.pixel = BusyBottom;
    o.Sample(0, picture);
    EXPECT_EQ(o.place(), presence::Place::Bottom) << "one sample is not enough";
    EXPECT_EQ(o.cost().read, 64u) << "a sample reads 32 pixels a band";
    o.Sample(500, picture);
    EXPECT_EQ(o.cost().read, 64u) << "once a second, no more";
    o.Sample(1000, picture);
    EXPECT_EQ(o.place(), presence::Place::Top);
    EXPECT_NE(o.Describe().find(" place=top"), std::string::npos) << o.Describe();
    ASSERT_TRUE(o.Configure("held place=bottom", error, 0));
    o.Sample(5000, picture);
    EXPECT_EQ(o.cost().read, 128u) << "a fixed place samples nothing";
}

TEST(PresenceRows, ARowTheScalerRewroteGetsTheOverlayOnceAndTheOthersKeepIt) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure(std::string("held") + kLook, error, 0));
    const Format f = Rgb32();
    o.Resize(20, 20, &f);
    o.Guest(10, 10);
    Frame frame(20, 20, 0x00000000u);
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, false, 0, 0);
    const uint32_t once = frame.at(0, 5);
    for (int n = 0; n < 3; n++) {
        o.UndrawRows(frame.buf(), frame.pitch(), 5, 1);   /* the scaler is about to write row 5 */
        EXPECT_EQ(frame.at(0, 5), 0x00000000u) << "row 5 holds the picture alone";
        EXPECT_EQ(frame.at(0, 6), once) << "row 6 keeps the overlay";
        for (unsigned x = 8; x < 12; x++) frame.px[5 * 20 + x] = 0x00102030u;   /* one block */
        const presence::Cost before = o.cost();
        const uint16_t changed[] = {5, 1, 14};
        EXPECT_EQ(o.Compose(frame.buf(), frame.pitch(), changed, 0, false, 0, 0), changed);
        EXPECT_EQ(frame.at(0, 5), once) << "blended once, however often the line changed";
        EXPECT_EQ(frame.at(0, 6), once);
        EXPECT_EQ(frame.at(10, 5), 0x00102030u);
        EXPECT_EQ(o.cost().read - before.read, 4u) << "the row's two strips, 2 wide each, saved";
    }
}

/* The look and the arrow the DOSBox-X server sends, so the cases price what a session draws. */
const char *const kSessionLook =
    " rgb=FF8C00 edge=0.025 steps=6 acting=0.850 held=0.283 fade=2.5 outline=2A1400 glide=0"
    " pointer=1 place=bottom caption_in=0 sprite=XX........./XoX......../XooX......./XoooX....../XooooX...../XoooooX...."
    "/XooooooX.../XoooooooX../XooooooooX./XoooooXXXXX/XooXooX..../XoXXXooX.../XX..XooX..."
    "/.....XooX../.....XooX../......XX...";

struct Output { unsigned w, h; };
/* Mode 13h doubled, and the same picture on a large window. */
const Output kOutputs[] = {{640, 400}, {1920, 1200}};
const unsigned kGuestW = 320, kGuestH = 200;

/* What one frame of a case costs, worked out from the geometry, not from the overlay's code. */
struct Expected {
    uint64_t band, edge, sx, box_w, box_h;
    explicit Expected(const Output &out) {
        band = (uint64_t)std::lround(0.025 * std::min(out.w, out.h));
        edge = (uint64_t)out.w * out.h - (uint64_t)(out.w - 2 * band) * (out.h - 2 * band);
        sx = out.w / kGuestW;
        box_w = 11 * sx;
        box_h = 16 * (out.h / kGuestH);
    }
    /* the covered pixels of a row in the edge's side strips */
    uint64_t strips() const { return 2 * band; }
};

presence::Cost Delta(const presence::Cost &after, const presence::Cost &before) {
    presence::Cost d;
    d.read = after.read - before.read;
    d.written = after.written - before.written;
    d.presented = after.presented - before.presented;
    return d;
}

void Print(const char *name, const Output &out, uint64_t frames, const presence::Cost &c) {
    std::printf("[presence-cost] case=%-7s output=%ux%u frames=%llu px_read=%llu"
                " px_written=%llu bytes_presented=%llu\n",
                name, out.w, out.h, (unsigned long long)frames, (unsigned long long)c.read,
                (unsigned long long)c.written, (unsigned long long)c.presented);
}

/* An overlay on a session's output with the arrow mid-picture, its first frame already shown,
   on an output that presents rectangles unless told otherwise. */
struct Session {
    Overlay o;
    Frame frame;
    explicit Session(const Output &out, const char *state = "held", bool columns = true)
        : frame(out.w, out.h, 0x00204060u) {
        std::string error;
        EXPECT_TRUE(o.Configure(std::string(state) + kSessionLook, error, 0)) << error;
        const Format f = Rgb32();
        o.Resize(out.w, out.h, &f, columns);
        o.Guest(kGuestW, kGuestH);
        o.Compose(frame.buf(), frame.pitch(), nullptr, 0, true, 0.5f, 0.5f);
    }
    /* A running guest's frame: the scaler takes the overlay off row 100, rewrites it, and the
       frame is drawn. */
    uint16_t one_line[3] = {100, 1, 0};
    const uint16_t *runs = nullptr;
    presence::Cost Running(uint32_t now_ms, float fx, float fy) {
        const presence::Cost before = o.cost();
        o.UndrawRows(frame.buf(), frame.pitch(), 100, 1);
        one_line[2] = (uint16_t)(frame.h - 101);
        runs = o.Compose(frame.buf(), frame.pitch(), one_line, now_ms, true, fx, fy);
        return Delta(o.cost(), before);
    }
    /* A frame the scaler did not write: the debugger holds the machine, or the picture is still. */
    presence::Cost Still(uint32_t now_ms, float fx = 0.5f, float fy = 0.5f) {
        const presence::Cost before = o.cost();
        runs = o.Compose(frame.buf(), frame.pitch(), nullptr, now_ms, true, fx, fy);
        return Delta(o.cost(), before);
    }
};

TEST(PresenceCost, TheReplyCarriesTheCounters) {
    Session s(kOutputs[0]);
    const presence::Cost &c = s.o.cost();
    const std::string line = s.o.Describe();
    EXPECT_NE(line.find(" px_read=" + std::to_string(c.read) + " "), std::string::npos) << line;
    EXPECT_NE(line.find(" px_written=" + std::to_string(c.written) + " "), std::string::npos) << line;
    EXPECT_NE(line.find(" bytes_presented=" + std::to_string(c.presented) + " "), std::string::npos) << line;
    EXPECT_NE(line.find(" uploads=0 upload_bytes=0 layers=edge,pointer,keys,calls,pause place=bottom"),
              std::string::npos) << line;
    EXPECT_GT(c.presented, 0u);
    const Format f = Rgb32();
    s.o.Resize(640, 400, &f);
    EXPECT_EQ(s.o.cost().read + s.o.cost().written + s.o.cost().presented, 0u)
        << "a new output starts its own count";
}

TEST(PresenceCost, AStillOverlayOnAStillPictureCostsNothing) {
    for (const Output &out : kOutputs) {
        Session s(out);
        EXPECT_FALSE(s.o.NeedsRepaint(0, true, 0.5f, 0.5f));
        const presence::Cost c = s.Still(0);
        EXPECT_EQ(s.runs, nullptr) << "nothing to present";
        EXPECT_EQ(c.read + c.written + c.presented, 0u);
    }
}

TEST(PresenceCost, AStillOverlayOnARunningGameTouchesOnlyTheRowTheScalerRewrote) {
    for (const Output &out : kOutputs) {
        Session s(out);
        const presence::Cost c = s.Running(0, 0.5f, 0.5f);
        Print("still", out, 1, c);
        const Expected e(out);
        EXPECT_EQ(c.read, e.strips()) << "the row's covered pixels, saved";
        EXPECT_EQ(c.written, 2 * e.strips()) << "put back, then blended";
        EXPECT_EQ(c.presented, 1ull * out.w * 4) << "only the scaler's row";
        EXPECT_EQ(s.runs, s.one_line);
    }
}

TEST(PresenceCost, AFadeFramePresentsTheEdgesPixelsAlone) {
    for (const Output &out : kOutputs) {
        Session s(out, "acting");
        std::string error;
        ASSERT_TRUE(s.o.Configure("held", error, 0));
        const presence::Cost c = s.Running(1250, 0.5f, 0.5f);
        Print("fade", out, 1, c);
        const Expected e(out);
        EXPECT_EQ(c.read, e.strips());
        EXPECT_EQ(c.written, e.strips() + e.edge) << "the row put back, the edge blended once";
        EXPECT_EQ(c.presented, 1ull * out.w * 4 + 4 * (e.edge - e.strips()))
            << "the scaler's row whole, the edge's pixels off it";
        const uint16_t *rows = nullptr;
        const std::vector<presence::Rect> *rects = nullptr;
        ASSERT_TRUE(s.o.Columns(s.runs, rows, rects));
        EXPECT_EQ(rows, s.one_line);
        uint64_t area = 0;
        for (const presence::Rect &r : *rects) {
            area += (uint64_t)(r.x1 - r.x0) * (r.y1 - r.y0);
            EXPECT_FALSE(r.y0 <= 100 && r.y1 > 100) << "no rectangle on the scaler's row";
        }
        EXPECT_EQ(area, e.edge - e.strips());
        EXPECT_FALSE(s.o.Columns(s.one_line, rows, rects)) << "only for the runs it handed on";
    }
}

TEST(PresenceCost, AFadeOnAnOutputOfRowsPresentsEveryRowTheEdgeLiesOn) {
    for (const Output &out : kOutputs) {
        Session s(out, "acting", false);
        std::string error;
        ASSERT_TRUE(s.o.Configure("held", error, 0));
        const presence::Cost c = s.Running(1250, 0.5f, 0.5f);
        Print("faderow", out, 1, c);
        EXPECT_EQ(c.presented, (uint64_t)out.w * out.h * 4) << "the side strips lie on every row";
        const uint16_t *rows = nullptr;
        const std::vector<presence::Rect> *rects = nullptr;
        EXPECT_FALSE(s.o.Columns(s.runs, rows, rects));
    }
}

TEST(PresenceCost, AMovingArrowPresentsItsTwoBoxes) {
    for (const Output &out : kOutputs) {
        Session s(out);
        const presence::Cost c = s.Running(0, 0.5f + 1.0f / kGuestW, 0.5f);
        Print("arrow", out, 1, c);
        const Expected e(out);
        const uint64_t both = (e.box_w + e.sx) * e.box_h;      /* one guest pixel apart */
        EXPECT_EQ(c.read, e.strips() + e.sx * e.box_h) << "the row, and the columns the arrow entered";
        EXPECT_EQ(c.written, 2 * e.strips() + both);
        EXPECT_EQ(c.presented, 1ull * out.w * 4 + 4 * both);
    }
}

TEST(PresenceCost, AParkedFadeRepaintsTheEdgeAtEveryAlphaStep) {
    for (const Output &out : kOutputs) {
        Session s(out, "acting");
        std::string error;
        ASSERT_TRUE(s.o.Configure("held", error, 0));
        const presence::Cost before = s.o.cost();
        uint64_t repaints = 0;
        for (uint32_t ms = 0; ms <= 3000; ms++) {      /* the parked loop ticks once a millisecond */
            if (!s.o.NeedsRepaint(ms, true, 0.5f, 0.5f)) continue;
            s.o.Compose(s.frame.buf(), s.frame.pitch(), nullptr, ms, true, 0.5f, 0.5f);
            repaints++;
        }
        const presence::Cost c = Delta(s.o.cost(), before);
        Print("parked", out, repaints, c);
        /* one alpha level at a time from acting's 54 to held's 18: the state's own change
           leaves the alpha where it was and repaints nothing */
        EXPECT_EQ(repaints, 54u - 18u);
        const Expected e(out);
        EXPECT_EQ(c.read, 0u) << "the blend reads the clean copy";
        EXPECT_EQ(c.written, repaints * e.edge);
        EXPECT_EQ(c.presented, repaints * 4 * e.edge);
    }
}

/* The output pixels of a guest box [gx0, gx1) by [gy0, gy1): each guest pixel's first output
   pixel is the first whose guest pixel it is. */
uint64_t OutSpan(uint64_t g0, uint64_t g1, uint64_t guest, uint64_t out) {
    return (g1 * out + guest - 1) / guest - (g0 * out + guest - 1) / guest;
}

TEST(PresenceCost, ACaptionIsDrawnOnceAndThenCostsNothing) {
    for (const Output &out : kOutputs) {
        Session s(out);
        std::string error;
        ASSERT_TRUE(s.o.Configure("held caption=start serial=1", error, 0));
        const presence::Cost c = s.Still(0);
        Print("caption", out, 1, c);
        const Expected e(out);
        /* "start" is 5 x 8 + 4 guest pixels, centred between insets of the edge's 5 plus 2 */
        const uint64_t gx0 = 7 + (320 - 14 - 44) / 2;
        const uint64_t w = OutSpan(gx0, gx0 + 44, kGuestW, out.w), h = OutSpan(177, 197, kGuestH, out.h);
        const uint64_t on_edge = w * (OutSpan(0, 197, kGuestH, out.h) - (out.h - e.band));
        EXPECT_EQ(c.written, w * h);
        EXPECT_EQ(c.read, w * h - on_edge) << "the box's pixels the edge had not saved already";
        EXPECT_EQ(c.presented, 4 * w * h);
        const presence::Cost later = s.Still(5000);
        EXPECT_EQ(later.read + later.written + later.presented, 0u) << "a still caption costs nothing";
    }
}

TEST(PresenceCost, AScrollingCaptionRepaintsItsBoxOncePerGuestPixel) {
    for (const Output &out : kOutputs) {
        Session s(out);
        std::string error;
        ASSERT_TRUE(s.o.Configure("held scroll_cps=12 serial=1 caption=" + std::string(60, 'x'), error, 0));
        s.Still(0);
        EXPECT_FALSE(s.o.NeedsRepaint(10, true, 0.5f, 0.5f));
        const presence::Cost c = s.Still(11);
        Print("scroll", out, 1, c);
        const uint64_t w = OutSpan(7, 313, kGuestW, out.w), h = OutSpan(177, 197, kGuestH, out.h);
        EXPECT_EQ(c.read, 0u);
        EXPECT_EQ(c.written, w * h) << "the box, as wide as the room";
        EXPECT_EQ(c.presented, 4 * w * h);
    }
}

} /* namespace */
