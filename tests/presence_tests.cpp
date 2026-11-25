/*
 *  Presence overlay (src/gui/render_presence.cpp): the command it takes, the
 *  pixel formats it can draw on, and what it does to an output buffer: an edge
 *  on the inner sides only, taken off before the scaler writes and drawn anew
 *  after, so a scaler that rewrites part of a line never gets the overlay
 *  blended onto itself; restored when turned off; an arrow at the guest's pointer; and
 *  the record mark. The cost cases print what each case costs and hold it at its floor.
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

extern uint8_t int10_font_08[256 * 8];
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
    EXPECT_NE(lines[0].find(" version=4"), std::string::npos) << lines[0];
    EXPECT_TRUE(PRESENCE_Command("  ", lines));
    EXPECT_NE(lines[0].find(" version=4"), std::string::npos) << lines[0];
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

TEST(Presence, TheArrowTurnedBackOnAppearsWhereItIsNotEasingInFromTheOldPlace) {
    Overlay o;
    std::string error;
    const std::string look = " rgb=FF8C00 edge=0.01 steps=1 acting=1 held=0.5 fade=1"
                             " outline=000000 sprite=Xo/oX glide=0.2";
    ASSERT_TRUE(o.Configure("held" + look + " pointer=1", error, 0)) << error;
    const Format f = Rgb32();
    o.Resize(100, 100, &f);
    o.Guest(100, 100);
    Frame frame(100, 100, 0x00FFFFFFu);
    o.Compose(frame.buf(), frame.pitch(), nullptr, 0, true, 0.25f, 0.25f);
    EXPECT_EQ(frame.at(25, 25), 0x00000000u) << "the arrow at its first place";
    ASSERT_TRUE(o.Configure("held" + look + " pointer=0", error, 0)) << error;
    o.Compose(frame.buf(), frame.pitch(), nullptr, 100, true, 0.25f, 0.25f);
    ASSERT_TRUE(o.Configure("held" + look + " pointer=1", error, 0)) << error;
    Frame next(100, 100, 0x00FFFFFFu);
    o.Compose(next.buf(), next.pitch(), nullptr, 200, true, 0.75f, 0.75f);
    EXPECT_EQ(next.at(75, 75), 0x00000000u) << "on again, it appears where it is";
    EXPECT_EQ(next.at(25, 25), 0x00FFFFFFu) << "not easing in from where it was";
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

/* A 320x200 output of a 320x200 guest, one output pixel a guest pixel, so every geometry these
   tests read is in guest pixels: `kInset` is the edge's own depth, where the mark sits. */
struct Band {
    Overlay o;
    Frame frame;
    explicit Band(const std::string &words, uint32_t picture = 0x00000000u) : frame(320, 200, picture) {
        std::string error;
        EXPECT_TRUE(o.Configure(words + " edge=0.025", error, 0)) << error;
        const Format f = Rgb32();
        o.Resize(320, 200, &f);
        o.Guest(320, 200);
    }
    void Draw(uint32_t now_ms) { o.Compose(frame.buf(), frame.pitch(), nullptr, now_ms, false, 0, 0); }
};
const unsigned kInset = 5;

/* The first lit pixel of a glyph in the VGA font `rows` tall, as (column, row). */
void FirstInk(unsigned char c, unsigned rows, unsigned &col, unsigned &row) {
    const uint8_t *font = rows == 16 ? int10_font_16 : int10_font_08;
    for (row = 0; row < rows; row++)
        for (col = 0; col < 8; col++)
            if (font[c * rows + row] & (0x80u >> col)) return;
}

/* The record mark's box: `kRecH` rows from `kRecY`, inset `kInset` from the right; its dot is
   `kRecDot` guest pixels, 2 inside the box and 3 rows down. */
const unsigned kRecY = kInset, kRecH = 12, kRecDot = 5, kRecDotY = kRecY + (kRecH - kRecDot) / 2;
const unsigned kRecVideoW = 2 + kRecDot + 2 + 3 * 8 + 2, kRecAudioW = 2 + kRecDot + 2 + 8 + 2;

TEST(PresenceLayers, TheRecordMarkBlinksInTheTopRightCorner) {
    Band b("held rec=video");
    b.Draw(0);
    unsigned x0 = 320 - kInset - kRecVideoW;
    EXPECT_EQ(b.frame.at(x0 + 2 + kRecDot / 2, kRecDotY + kRecDot / 2), 0x00FF3B30u)
        << "the dot, record_rgb";
    const uint32_t cap = b.frame.at(x0 + 2, kRecDotY);
    EXPECT_NE(cap, 0u) << "the dot's corner is the box";
    EXPECT_NE(cap, 0x00FF3B30u) << "and not the dot: a disc";
    EXPECT_EQ(b.frame.at(x0 - 1, kRecY), 0u);
    EXPECT_NE(b.frame.at(x0, kRecY - 1), 0u) << "the edge above the mark";
    unsigned col = 0, row = 0;
    FirstInk('R', 8, col, row);
    EXPECT_EQ(b.frame.at(x0 + 2 + kRecDot + 2 + col, kRecY + 2 + row), 0x00FFFFFFu) << "REC";
    EXPECT_FALSE(b.o.NeedsRepaint(499, false, 0, 0)) << "on for the first half of the second";
    EXPECT_TRUE(b.o.NeedsRepaint(500, false, 0, 0));
    b.Draw(500);
    EXPECT_EQ(b.frame.at(x0 + 2 + kRecDot / 2, kRecDotY + kRecDot / 2), 0u) << "off for the second half";
    b.Draw(1000);
    EXPECT_EQ(b.frame.at(x0 + 2 + kRecDot / 2, kRecDotY + kRecDot / 2), 0x00FF3B30u);

    std::string error;
    ASSERT_TRUE(b.o.Configure("held rec=audio", error, 1200));
    b.Draw(1200);
    EXPECT_EQ(b.frame.at(x0 + 2 + kRecDot / 2, kRecDotY + kRecDot / 2), 0u) << "the video mark gone";
    x0 = 320 - kInset - kRecAudioW;
    EXPECT_EQ(b.frame.at(x0 + 2 + kRecDot / 2, kRecDotY + kRecDot / 2), 0x00FF3B30u) << "a change shows at once";
    FirstInk(0x0E, 8, col, row);
    EXPECT_EQ(b.frame.at(x0 + 2 + kRecDot + 2 + col, kRecY + 2 + row), 0x00FFFFFFu) << "the note";

    ASSERT_TRUE(b.o.Configure("held rec=both", error, 2000));
    b.Draw(2000);
    x0 = 320 - kInset - (kRecVideoW + 2 + kRecAudioW - 2 * 2);
    FirstInk('R', 8, col, row);
    EXPECT_EQ(b.frame.at(x0 + 2 + kRecDot + 2 + col, kRecY + 2 + row), 0x00FFFFFFu) << "REC first";
    const unsigned audio_x0 = x0 + kRecVideoW - 2 + 2;
    FirstInk(0x0E, 8, col, row);
    EXPECT_EQ(b.frame.at(audio_x0 + kRecDot + 2 + col, kRecY + 2 + row), 0x00FFFFFFu) << "then the note";

    ASSERT_TRUE(b.o.Configure("held rec=video record_blink_s=2", error, 3000));
    b.Draw(3000);
    EXPECT_FALSE(b.o.NeedsRepaint(3999, false, 0, 0)) << "the period the look gives";
    EXPECT_TRUE(b.o.NeedsRepaint(4000, false, 0, 0));

    x0 = 320 - kInset - kRecVideoW;
    EXPECT_EQ(b.frame.at(x0 + 2 + kRecDot / 2, kRecDotY + kRecDot / 2), 0x00FF3B30u) << "the video mark, on";
    ASSERT_TRUE(b.o.Configure("held rec=none", error, 3500));
    b.Draw(3500);
    EXPECT_EQ(b.frame.at(x0 + 2 + kRecDot / 2, kRecDotY + kRecDot / 2), 0u) << "taken off mid blink";
    EXPECT_FALSE(b.o.NeedsRepaint(9000, false, 0, 0)) << "nothing recorded, nothing blinks";
}

TEST(PresenceLayers, TheLayersWordTurnsLayersOff) {
    Band b("held rec=video layers=edge");
    b.Draw(0);
    const unsigned x0 = 320 - kInset - kRecVideoW;
    EXPECT_EQ(b.frame.at(x0 + 2 + kRecDot / 2, kRecDotY + kRecDot / 2), 0u) << "no record layer asked";
    EXPECT_NE(b.frame.at(0, 100), 0u) << "the edge is always drawn";
    EXPECT_NE(b.o.Describe().find(" layers=edge"), std::string::npos) << b.o.Describe();
}

TEST(PresenceLayers, AnOutputItCannotDrawOnSaysWhichLayersItLacks) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure("held layers=edge,record", error, 0));
    o.Resize(20, 20, nullptr);
    EXPECT_NE(o.Describe().find(" output=undrawable layers= lacks=edge,record"),
              std::string::npos) << o.Describe();
}

TEST(PresenceLayers, TheProtocolRefusesWhatItCannotDrawAndChangesNothing) {
    Overlay o;
    std::string error;
    ASSERT_TRUE(o.Configure("held rec=video", error, 0)) << error;
    EXPECT_FALSE(o.Configure("held layers=edge,captions", error, 0)) << "an unknown layer";
    EXPECT_FALSE(o.Configure("held caption=run", error, 0)) << "the old narration, refused by name";
    EXPECT_FALSE(o.Configure("held keys=Esc", error, 0));
    EXPECT_FALSE(o.Configure("held place=top", error, 0));
    EXPECT_FALSE(o.Configure("held tint=red", error, 0));
    EXPECT_FALSE(o.Configure("held key_alpha=100", error, 0));
    EXPECT_FALSE(o.Configure("paused", error, 0)) << "no pause state";
    EXPECT_FALSE(o.Configure("paused rec=screen", error, 0)) << "an unknown recording";
    EXPECT_FALSE(o.Configure("held rec=", error, 0));
    EXPECT_FALSE(o.Configure("held record_rgb=red", error, 0));
    EXPECT_FALSE(o.Configure("held record_blink_s=0", error, 0));
    EXPECT_FALSE(o.Configure("held record_blink_s=11", error, 0));
    EXPECT_TRUE(o.Configure("held record_blink_s=0.1", error, 0)) << "a bound is a value it takes";
    EXPECT_TRUE(o.Configure("held record_blink_s=10", error, 0));
    EXPECT_EQ(o.state(), State::Held) << "a refused command changes nothing";
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
    " pointer=1 sprite=XX........./XoX......../XooX......./XoooX....../XooooX...../XoooooX...."
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
    d.uploads = after.uploads - before.uploads;
    d.upload_bytes = after.upload_bytes - before.upload_bytes;
    return d;
}

void Print(const char *name, const Output &out, uint64_t frames, const presence::Cost &c) {
    std::printf("[presence-cost] case=%-7s output=%ux%u frames=%llu px_read=%llu"
                " px_written=%llu bytes_presented=%llu uploads=%llu upload_bytes=%llu\n",
                name, out.w, out.h, (unsigned long long)frames, (unsigned long long)c.read,
                (unsigned long long)c.written, (unsigned long long)c.presented,
                (unsigned long long)c.uploads, (unsigned long long)c.upload_bytes);
}

/* An overlay on a session's output with the arrow mid-picture, its first frame already shown,
   on an output that presents rectangles unless told otherwise. */
struct Session {
    Overlay o;
    Frame frame;
    explicit Session(const Output &out, const char *state = "held", bool columns = true,
                     presence::Texture *texture = nullptr)
        : frame(out.w, out.h, 0x00204060u) {
        std::string error;
        EXPECT_TRUE(o.Configure(std::string(state) + kSessionLook, error, 0)) << error;
        const Format f = Rgb32();
        o.Resize(out.w, out.h, &f, columns, texture);
        o.Guest(kGuestW, kGuestH);
        o.Compose(frame.buf(), frame.pitch(), nullptr, 0, true, 0.5f, 0.5f);
        o.Upload();
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
    EXPECT_NE(line.find(" uploads=0 upload_bytes=0 layers=edge,pointer,record"),
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

/* The video mark's box in output pixels, as kRecY and kRecVideoW lay it in the guest. */
void RecBox(const Output &out, uint64_t &w, uint64_t &h) {
    w = OutSpan(320 - kInset - kRecVideoW, 320 - kInset, kGuestW, out.w);
    h = OutSpan(kRecY, kRecY + kRecH, kGuestH, out.h);
}

TEST(PresenceCost, ABlinkRepaintsAndPresentsTheRecordMarksBoxAlone) {
    for (const Output &out : kOutputs) {
        Session s(out);
        std::string error;
        ASSERT_TRUE(s.o.Configure("held rec=video", error, 0));
        uint64_t w = 0, h = 0;
        RecBox(out, w, h);
        const presence::Cost shown = s.Still(0);
        Print("rec", out, 1, shown);
        EXPECT_EQ(shown.read, w * h) << "the box's pixels saved";
        EXPECT_EQ(shown.written, w * h);
        EXPECT_EQ(shown.presented, 4 * w * h);
        const presence::Cost before = s.o.cost();
        uint64_t repaints = 0;
        for (uint32_t ms = 1; ms <= 3000; ms++) {      /* the parked loop, once a millisecond */
            if (!s.o.NeedsRepaint(ms, true, 0.5f, 0.5f)) continue;
            s.Still(ms);
            repaints++;
        }
        const presence::Cost c = Delta(s.o.cost(), before);
        Print("recblink", out, repaints, c);
        EXPECT_EQ(repaints, 6u) << "off and on again, each half second";
        EXPECT_EQ(c.read, 3 * w * h) << "saved again each time it comes back";
        EXPECT_EQ(c.written, repaints * w * h) << "put back, or blended";
        EXPECT_EQ(c.presented, repaints * 4 * w * h) << "the box alone";
        const uint16_t *rows = nullptr;
        const std::vector<presence::Rect> *rects = nullptr;
        ASSERT_TRUE(s.o.Columns(s.runs, rows, rects));
        ASSERT_EQ(rects->size(), 1u);
        EXPECT_EQ((uint64_t)((*rects)[0].x1 - (*rects)[0].x0) * ((*rects)[0].y1 - (*rects)[0].y0), w * h);
        ASSERT_TRUE(s.o.Configure("held rec=none", error, 3000));
        s.Still(3000);
        const presence::Cost later = s.Still(10000);
        EXPECT_EQ(later.read + later.written + later.presented, 0u) << "no recording costs nothing";
    }
}

/* An output's texture in memory, as the fake backend of decision 15 (b): what the texture would
   hold, and what it was asked to do. Allocated garbage, so a pixel never uploaded shows. */
struct FakeTexture final : presence::Texture {
    unsigned w = 0, h = 0;
    std::vector<uint32_t> px;
    unsigned allocations = 0, uploads = 0;
    void Allocate(unsigned width, unsigned height) override {
        w = width;
        h = height;
        px.assign((size_t)w * h, 0xDEADBEEFu);
        allocations++;
    }
    void Upload(const uint32_t *pixels, size_t pitch, const presence::Rect &r) override {
        ASSERT_TRUE(r.x1 <= w && r.y1 <= h && !r.empty());
        for (unsigned y = r.y0; y < r.y1; y++)
            for (unsigned x = r.x0; x < r.x1; x++) px[(size_t)y * w + x] = pixels[(size_t)y * pitch + x];
        uploads++;
    }
    void Present() override {}
};

/* Everything the session draws at once: the edge, the arrow and the record mark. */
const char *const kEveryLayer = " rec=both";

/* The same overlay drawn on the CPU over black and over white, and as a texture: the texture
   holds premultiplied ARGB, so over black the CPU path shows its colour, and over white what is
   left of the white is its alpha. */
struct ThreeWays {
    FakeTexture texture;             /* before the session that draws into it */
    Session black, white, tex;
    explicit ThreeWays(const char *state)
        : black(kOutputs[0], state, true), white(kOutputs[0], state, true),
          tex(kOutputs[0], state, true, &texture) {
        std::fill(black.frame.px.begin(), black.frame.px.end(), 0x00000000u);
        std::fill(white.frame.px.begin(), white.frame.px.end(), 0x00FFFFFFu);
        Draw(0, "", 0.5f, 0.5f);
    }
    /* Every row rewritten by the scaler, so the CPU path draws the whole overlay afresh. */
    void Draw(uint32_t now_ms, const std::string &words, float fx, float fy) {
        for (Session *s : {&black, &white, &tex}) {
            std::string error;
            if (!words.empty()) ASSERT_TRUE(s->o.Configure(words, error, now_ms)) << error;
            s->o.Undraw(s->frame.buf(), s->frame.pitch());
            if (s == &black) std::fill(s->frame.px.begin(), s->frame.px.end(), 0x00000000u);
            if (s == &white) std::fill(s->frame.px.begin(), s->frame.px.end(), 0x00FFFFFFu);
            s->o.Compose(s->frame.buf(), s->frame.pitch(), nullptr, now_ms, true, fx, fy);
            s->o.Upload();
        }
    }
    /* How many pixels differ from what the CPU path drew, and how many the overlay covers. */
    void Compare(unsigned &wrong, unsigned &covered) const {
        wrong = covered = 0;
        ASSERT_EQ(texture.px.size(), black.frame.px.size());
        for (size_t i = 0; i < texture.px.size(); i++) {
            const uint32_t t = texture.px[i], a = t >> 24;
            const uint32_t left = 255u * (256u - (a + (a >> 7))) >> 8;   /* of white, under alpha a */
            const uint32_t on_white = white.frame.px[i];
            bool ok = (t & 0xFFFFFFu) == black.frame.px[i];
            for (unsigned c = 0; c < 3; c++) {
                const uint32_t premul = (t >> (8 * c)) & 0xFF;
                ok = ok && premul <= a && ((on_white >> (8 * c)) & 0xFF) == premul + left;
            }
            wrong += !ok;
            covered += a != 0;
        }
    }
};

TEST(PresenceCompositor, TheTextureHoldsEveryLayerAsTheCpuPathBlendsIt) {
    ThreeWays w((std::string("held") + kEveryLayer).c_str());
    unsigned wrong = 0, covered = 0;
    w.Compare(wrong, covered);
    EXPECT_EQ(wrong, 0u);
    const Expected e(kOutputs[0]);
    EXPECT_GT(covered, e.edge) << "the edge and the band's boxes and the arrow";
    EXPECT_EQ(w.texture.allocations, 1u);
    EXPECT_EQ(w.texture.w, kOutputs[0].w);
    EXPECT_EQ(w.texture.h, kOutputs[0].h);

    /* a fade step, the arrow moved, the blink on: uploaded only where they changed */
    w.Draw(0, "held" + std::string(kSessionLook), 0.25f, 0.75f);
    w.Draw(1250, "", 0.25f, 0.75f);
    w.Compare(wrong, covered);
    EXPECT_EQ(wrong, 0u);
    EXPECT_EQ(w.texture.allocations, 1u);
}

TEST(PresenceCompositor, TheTexturePathNeverTouchesTheFrame) {
    FakeTexture texture;
    Session s(kOutputs[0], "acting", true, &texture);
    const std::vector<uint32_t> picture = Frame(kOutputs[0].w, kOutputs[0].h, 0x00204060u).px;
    EXPECT_TRUE(s.frame.px == picture) << "the first frame drawn left it as the scaler wrote it";
    std::string error;
    ASSERT_TRUE(s.o.Configure(std::string("held") + kEveryLayer, error, 0));
    EXPECT_FALSE(s.o.in_buffer()) << "the scaler has nothing to take off a line";
    s.o.UndrawRows(s.frame.buf(), s.frame.pitch(), 0, kOutputs[0].h);
    s.o.Undraw(s.frame.buf(), s.frame.pitch());
    s.Running(500, 0.75f, 0.25f);
    EXPECT_EQ(s.runs, s.one_line) << "the scaler's own rows are presented, and nothing else";
    const uint16_t *rows = nullptr;
    const std::vector<presence::Rect> *rects = nullptr;
    EXPECT_FALSE(s.o.Columns(s.runs, rows, rects));
    s.Still(3000);
    EXPECT_EQ(s.runs, nullptr);
    EXPECT_TRUE(s.frame.px == picture);
    EXPECT_TRUE(s.o.drawn());
    EXPECT_GT(s.o.cost().upload_bytes, 0u);
}

TEST(PresenceTexture, AStillOverlayOnARunningGameCostsNothing) {
    for (const Output &out : kOutputs) {
        FakeTexture texture;
        Session s(out, "held", true, &texture);
        const presence::Cost before = s.o.cost();
        s.Running(0, 0.5f, 0.5f);
        s.o.Upload();
        const presence::Cost c = Delta(s.o.cost(), before);
        Print("tstill", out, 1, c);
        EXPECT_EQ(c.read + c.written + c.presented + c.uploads + c.upload_bytes, 0u);
        EXPECT_FALSE(s.o.NeedsRepaint(0, true, 0.5f, 0.5f));
        EXPECT_FALSE(s.o.upload_pending());
    }
}

TEST(PresenceTexture, AFadeUploadsTheEdgesPixelsAlone) {
    for (const Output &out : kOutputs) {
        FakeTexture texture;
        Session s(out, "acting", true, &texture);
        std::string error;
        ASSERT_TRUE(s.o.Configure("held", error, 0));
        const presence::Cost before = s.o.cost();
        s.Running(1250, 0.5f, 0.5f);
        EXPECT_TRUE(s.o.upload_pending()) << "a frame with no new lines is still to be shown";
        s.o.Upload();
        const presence::Cost c = Delta(s.o.cost(), before);
        Print("tfade", out, 1, c);
        const Expected e(out);
        EXPECT_EQ(c.read + c.written + c.presented, 0u) << "the frame is not touched or re-sent";
        EXPECT_EQ(c.upload_bytes, 4 * e.edge);
        EXPECT_EQ(c.uploads, 4u) << "the top and bottom bands, and the two side strips";
        EXPECT_NE(s.o.Describe().find(" uploads=" + std::to_string(s.o.cost().uploads) +
                                      " upload_bytes=" + std::to_string(s.o.cost().upload_bytes) + " "),
                  std::string::npos) << s.o.Describe();
    }
}

TEST(PresenceTexture, AMovingArrowUploadsItsTwoBoxes) {
    for (const Output &out : kOutputs) {
        FakeTexture texture;
        Session s(out, "held", true, &texture);
        const presence::Cost before = s.o.cost();
        s.Running(0, 0.5f + 1.0f / kGuestW, 0.5f);
        s.o.Upload();
        const presence::Cost c = Delta(s.o.cost(), before);
        Print("tarrow", out, 1, c);
        const Expected e(out);
        EXPECT_EQ(c.read + c.written + c.presented, 0u);
        EXPECT_EQ(c.upload_bytes, 4 * (e.box_w + e.sx) * e.box_h) << "the union of the two boxes";
        EXPECT_EQ(c.uploads, 1u);
    }
}

TEST(PresenceTexture, AParkedFadeUploadsTheEdgeAtEveryAlphaStepAndPresentsNoFrame) {
    for (const Output &out : kOutputs) {
        FakeTexture texture;
        Session s(out, "acting", true, &texture);
        std::string error;
        ASSERT_TRUE(s.o.Configure("held", error, 0));
        const presence::Cost before = s.o.cost();
        uint64_t repaints = 0;
        for (uint32_t ms = 0; ms <= 3000; ms++) {      /* the parked loop, as PRESENCE_Repaint */
            if (!s.o.NeedsRepaint(ms, true, 0.5f, 0.5f)) continue;
            s.o.Compose(nullptr, 0, nullptr, ms, true, 0.5f, 0.5f);
            s.o.Upload();
            repaints++;
        }
        const presence::Cost c = Delta(s.o.cost(), before);
        Print("tparked", out, repaints, c);
        const Expected e(out);
        EXPECT_EQ(repaints, 54u - 18u);
        EXPECT_EQ(c.read + c.written + c.presented, 0u);
        EXPECT_EQ(c.upload_bytes, repaints * 4 * e.edge);
    }
}

TEST(PresenceTexture, ABlinkUploadsTheRecordMarksBoxAloneAndPresentsNoFrame) {
    for (const Output &out : kOutputs) {
        FakeTexture texture;
        Session s(out, "held", true, &texture);
        std::string error;
        ASSERT_TRUE(s.o.Configure("held rec=video", error, 0));
        uint64_t w = 0, h = 0;
        RecBox(out, w, h);
        const presence::Cost before = s.o.cost();
        uint64_t repaints = 0;
        for (uint32_t ms = 0; ms <= 3000; ms++) {      /* the parked loop, as PRESENCE_Repaint */
            if (!s.o.NeedsRepaint(ms, true, 0.5f, 0.5f)) continue;
            s.o.Compose(nullptr, 0, nullptr, ms, true, 0.5f, 0.5f);
            s.o.Upload();
            repaints++;
        }
        const presence::Cost c = Delta(s.o.cost(), before);
        Print("trec", out, repaints, c);
        EXPECT_EQ(repaints, 7u) << "shown, then off and on again each half second";
        EXPECT_EQ(c.read + c.written + c.presented, 0u);
        EXPECT_EQ(c.uploads, repaints);
        EXPECT_EQ(c.upload_bytes, repaints * 4 * w * h) << "the box alone";
        ASSERT_TRUE(s.o.Configure("held rec=none", error, 3000));
        s.o.Compose(nullptr, 0, nullptr, 3000, true, 0.5f, 0.5f);
        s.o.Upload();
        const presence::Cost mid = s.o.cost();
        for (uint32_t ms = 3001; ms <= 6000; ms++) EXPECT_FALSE(s.o.NeedsRepaint(ms, true, 0.5f, 0.5f));
        EXPECT_EQ(Delta(s.o.cost(), mid).uploads, 0u);
    }
}

TEST(PresenceTexture, ANewOrLostTextureIsUploadedWholeOnce) {
    FakeTexture texture;
    const Output &out = kOutputs[0];
    Session s(out, "held", true, &texture);
    EXPECT_EQ(texture.allocations, 1u);
    EXPECT_EQ(s.o.cost().upload_bytes, 4ull * out.w * out.h) << "a new texture holds nothing yet";
    const presence::Cost before = s.o.cost();
    s.o.TextureLost();
    EXPECT_TRUE(s.o.NeedsRepaint(0, true, 0.5f, 0.5f)) << "the overlay must come back";
    s.Still(0);
    s.o.Upload();
    const presence::Cost c = Delta(s.o.cost(), before);
    EXPECT_EQ(texture.allocations, 2u);
    EXPECT_EQ(c.uploads, 1u);
    EXPECT_EQ(c.upload_bytes, 4ull * out.w * out.h);
    EXPECT_EQ(std::count(texture.px.begin(), texture.px.end(), 0xDEADBEEFu), 0)
        << "every pixel of the new texture uploaded";

    /* lost while nothing is drawn: nothing to upload until something is */
    std::string error;
    ASSERT_TRUE(s.o.Configure("off", error, 0));
    s.Still(0);
    s.o.Upload();
    const presence::Cost off = s.o.cost();
    s.o.TextureLost();
    EXPECT_FALSE(s.o.NeedsRepaint(0, true, 0.5f, 0.5f));
    s.o.Upload();
    EXPECT_EQ(Delta(s.o.cost(), off).uploads, 0u);
    EXPECT_EQ(texture.allocations, 2u);
}

} /* namespace */
