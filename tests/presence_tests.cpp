/*
 *  Presence overlay (src/gui/render_presence.cpp): the command it takes, the
 *  pixel formats it can draw on, and what it does to an output buffer: an edge
 *  on the inner sides only, taken off before the scaler writes and drawn anew
 *  after, so a scaler that rewrites part of a line never gets the overlay
 *  blended onto itself; restored when turned off; and an arrow at the guest's
 *  pointer. The cost cases print the baseline the overlay's redesign is judged against.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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
    EXPECT_NE(lines[0].find(" version=2"), std::string::npos) << lines[0];
    EXPECT_TRUE(PRESENCE_Command("  ", lines));
    EXPECT_NE(lines[0].find(" version=2"), std::string::npos) << lines[0];
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

/* The look and the arrow the DOSBox-X server sends, so the baseline prices what a session draws. */
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
    uint64_t edge, arrow_box, arrow_ink;
    Expected(const Output &out) {
        const unsigned band = (unsigned)std::lround(0.025 * std::min(out.w, out.h));
        edge = (uint64_t)out.w * out.h - (uint64_t)(out.w - 2 * band) * (out.h - 2 * band);
        const unsigned sx = out.w / kGuestW, sy = out.h / kGuestH;
        arrow_box = 11ull * sx * 16ull * sy;
        uint64_t ink = 0;
        for (const char *c = std::strstr(kSessionLook, "sprite=") + 7; *c; c++) ink += *c == 'X' || *c == 'o';
        arrow_ink = ink * sx * sy;
    }
    /* undraw what was covered, save it again, blend the edge, paint the arrow */
    uint64_t read() const { return edge + arrow_box + edge; }
    uint64_t written() const { return edge + arrow_box + edge + arrow_ink; }
};

presence::Cost Delta(const presence::Cost &after, const presence::Cost &before) {
    presence::Cost d;
    d.read = after.read - before.read;
    d.written = after.written - before.written;
    d.presented = after.presented - before.presented;
    return d;
}

void Print(const char *name, const Output &out, uint64_t frames, const presence::Cost &c) {
    std::printf("[presence-baseline] case=%-7s output=%ux%u frames=%llu px_read=%llu"
                " px_written=%llu bytes_presented=%llu\n",
                name, out.w, out.h, (unsigned long long)frames, (unsigned long long)c.read,
                (unsigned long long)c.written, (unsigned long long)c.presented);
}

/* An overlay on a session's output with the arrow mid-picture, its first frame already shown. */
struct Session {
    Overlay o;
    Frame frame;
    explicit Session(const Output &out, const char *state = "held") : frame(out.w, out.h, 0x00204060u) {
        std::string error;
        EXPECT_TRUE(o.Configure(std::string(state) + kSessionLook, error, 0)) << error;
        const Format f = Rgb32();
        o.Resize(out.w, out.h, &f);
        o.Guest(kGuestW, kGuestH);
        o.Compose(frame.buf(), frame.pitch(), nullptr, 0, true, 0.5f, 0.5f);
    }
    /* A running guest's frame: the scaler takes the overlay off, rewrites one line, and it is drawn. */
    presence::Cost Running(uint32_t now_ms, float fx, float fy) {
        const presence::Cost before = o.cost();
        o.Undraw(frame.buf(), frame.pitch());
        const uint16_t one_line[] = {100, 1, (uint16_t)(frame.h - 101)};
        o.Compose(frame.buf(), frame.pitch(), one_line, now_ms, true, fx, fy);
        return Delta(o.cost(), before);
    }
};

TEST(PresenceCost, TheReplyCarriesTheCounters) {
    Session s(kOutputs[0]);
    const presence::Cost &c = s.o.cost();
    const std::string line = s.o.Describe();
    EXPECT_NE(line.find(" px_read=" + std::to_string(c.read) + " "), std::string::npos) << line;
    EXPECT_NE(line.find(" px_written=" + std::to_string(c.written) + " "), std::string::npos) << line;
    EXPECT_NE(line.find(" bytes_presented=" + std::to_string(c.presented)), std::string::npos) << line;
    EXPECT_GT(c.presented, 0u);
    const Format f = Rgb32();
    s.o.Resize(640, 400, &f);
    EXPECT_EQ(s.o.cost().read + s.o.cost().written + s.o.cost().presented, 0u)
        << "a new output starts its own count";
}

TEST(PresenceCost, AStillOverlayOnARunningGameIsTakenOffAndDrawnAgainEveryFrame) {
    for (const Output &out : kOutputs) {
        Session s(out);
        const presence::Cost c = s.Running(0, 0.5f, 0.5f);
        Print("still", out, 1, c);
        const Expected e(out);
        EXPECT_EQ(c.read, e.read());
        EXPECT_EQ(c.written, e.written());
        EXPECT_EQ(c.presented, 1ull * out.w * 4) << "only the scaler's line";
    }
}

TEST(PresenceCost, AFadeFramePresentsTheWholeFrame) {
    for (const Output &out : kOutputs) {
        Session s(out, "acting");
        std::string error;
        ASSERT_TRUE(s.o.Configure("held", error, 0));
        const presence::Cost c = s.Running(1250, 0.5f, 0.5f);
        Print("fade", out, 1, c);
        const Expected e(out);
        EXPECT_EQ(c.read, e.read());
        EXPECT_EQ(c.written, e.written());
        EXPECT_EQ(c.presented, (uint64_t)out.w * out.h * 4);
    }
}

TEST(PresenceCost, AMovingArrowPresentsTheWholeFrame) {
    for (const Output &out : kOutputs) {
        Session s(out);
        const presence::Cost c = s.Running(0, 0.5f + 1.0f / kGuestW, 0.5f);
        Print("arrow", out, 1, c);
        const Expected e(out);
        EXPECT_EQ(c.read, e.read());
        EXPECT_EQ(c.written, e.written());
        EXPECT_EQ(c.presented, (uint64_t)out.w * out.h * 4);
    }
}

TEST(PresenceCost, ThereIsNoCaptionToChangeYet) {
    for (const Output &out : kOutputs) {
        Session s(out);
        std::string error;
        EXPECT_FALSE(s.o.Configure("held caption=start", error, 0)) << "a caption layer exists now";
    }
}

TEST(PresenceCost, AParkedFadeRepaintsTheWholeFrameAtEveryAlphaStep) {
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
        /* the state's own change, then one alpha level at a time from acting's 54 to held's 18 */
        EXPECT_EQ(repaints, 1u + (54u - 18u));
        const Expected e(out);
        EXPECT_EQ(c.read, repaints * e.read());
        EXPECT_EQ(c.written, repaints * e.written());
        EXPECT_EQ(c.presented, repaints * out.w * out.h * 4);
    }
}

} /* namespace */
