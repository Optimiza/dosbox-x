/*
 *  Presence indicator: a client driving the emulator through the mcp_server
 *  debugger channel marks the window it drives (a soft edge on the output's
 *  inner sides and, once asked, an arrow at the guest's mouse position).
 *
 *  It is drawn into the scaled output buffer just before it is presented,
 *  after the frame has been handed to capture: screenshots, video capture and
 *  the VGA memory never see it. See README.debugger, "Presence".
 */

#ifndef DOSBOX_RENDER_PRESENCE_H
#define DOSBOX_RENDER_PRESENCE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace presence {

enum class State { Off, Held, Acting };

/* How the overlay is drawn: everything comes from the client, so the emulator
   holds no look of its own. Colours are 0xRRGGBB, alphas and sizes 0 to 1. */
struct Look {
    uint32_t rgb = 0xFF8C00;
    uint32_t outline = 0x2A1400;
    float edge = 0.025f;      /* edge width, a fraction of the shorter side */
    int steps = 6;            /* bands of the edge, strongest at the rim */
    float acting = 0.85f;
    float held = 0.28f;
    float fade = 2.5f;        /* seconds from acting back to held */
    float size = 0.06f;       /* arrow height, a fraction of the shorter side */
    float glide = 0.2f;       /* seconds the arrow takes to ease out to a new place */
    /* the arrow, top row first: 'X' outline, 'o' fill, '.' untouched */
    std::vector<std::string> sprite;
    bool operator==(const Look &o) const {
        return rgb == o.rgb && outline == o.outline && edge == o.edge && steps == o.steps &&
               acting == o.acting && held == o.held && fade == o.fade && size == o.size &&
               glide == o.glide && sprite == o.sprite;
    }
};

/* Where each of red, green and blue sits in an output pixel of `bytes` bytes,
   and the bits every opaque pixel carries besides them (an alpha byte). */
struct Format {
    unsigned bytes = 0;
    uint32_t base = 0;
    uint32_t mask[3] = {0, 0, 0};
    unsigned shift[3] = {0, 0, 0};
    uint32_t max[3] = {0, 0, 0};
};

/* The format of an output whose black, red, green and blue are `black`, `r`,
   `g`, `b`, as its own colour function builds them; false if each primary is
   not one run of bits of its own. */
bool FormatFromPixels(unsigned bytes, uint32_t black, uint32_t r, uint32_t g, uint32_t b,
                      Format &out);

class Overlay {
public:
    /* "<off|held|acting> [key=value ...]": the state, the look and whether the
       arrow is drawn (pointer=0|1). Keys left out keep their value. `now_ms` is
       the host clock a fade from acting is timed on. */
    bool Configure(const std::string &args, std::string &error, uint32_t now_ms);

    /* The output the overlay is drawn on; `fmt` null when it cannot be (an
       8-bit or text output, no output yet). A new output holds no overlay. */
    void Resize(unsigned width, unsigned height, const Format *fmt);

    /* The buffer is about to be written by the scaler: the pixels the overlay
       covers are put back, so the buffer holds only the emulated picture. */
    void Undraw(uint8_t *buf, size_t pitch);

    /* Whether the next Compose would change what is on screen. */
    bool NeedsRepaint(uint32_t now_ms, bool have_pointer, float fx, float fy) const;

    /* Draw onto `buf`, the output buffer holding the frame about to be shown,
       of which the scaler presents the line runs `changed` (its alternating
       unchanged/changed counts; nullptr when it wrote nothing). Returns the line
       runs to present: `changed` when the overlay stayed as it was, or the
       whole frame when it moved. `shown` is false for a frame that is drawn on
       but never presented (an aborted one), so what it held is drawn again. */
    const uint16_t *Compose(uint8_t *buf, size_t pitch, const uint16_t *changed,
                            uint32_t now_ms, bool have_pointer, float fx, float fy,
                            bool shown = true);

    State state() const { return state_; }
    bool drawn() const { return drawn_; }
    uint64_t frames() const { return frames_; }
    bool drawable() const { return fmt_.bytes != 0 && width_ != 0 && height_ != 0; }
    /* One line for the channel's reply. */
    std::string Describe() const;

private:
    struct Key {
        State state = State::Off;
        int alpha = -1;
        bool arrow = false;
        int ax = 0, ay = 0;
        unsigned look = 0;
        bool operator==(const Key &o) const {
            return state == o.state && alpha == o.alpha && arrow == o.arrow && ax == o.ax &&
                   ay == o.ay && look == o.look;
        }
    };
    /* A run of one row's pixels the overlay covers: [x0, x1) of row y. */
    struct Span { unsigned y, x0, x1; };

    Key KeyAt(uint32_t now_ms, bool have_pointer, float fx, float fy) const;
    /* Where the arrow is drawn at `now_ms`, as a fraction of the output: gliding
       from where it was drawn to the pointer at (fx, fy), the first time there. */
    void ArrowAt(uint32_t now_ms, float fx, float fy, float &x, float &y) const;
    int Band() const;
    int ArrowScale() const;
    std::vector<Span> Covered(const Key &key) const;
    void Draw(uint8_t *buf, size_t pitch, const Key &key) const;
    uint32_t Blend(uint32_t under, const uint32_t c8[3], int alpha256) const;
    uint32_t Pixel(uint32_t rgb) const;

    Look look_;
    unsigned look_generation_ = 0;
    State state_ = State::Off;
    bool pointer_ = false;
    uint32_t since_ms_ = 0;          /* when the state last changed */
    State faded_from_ = State::Off;  /* the state before it, which a fade starts from */
    unsigned width_ = 0, height_ = 0;
    Format fmt_;
    /* each channel's value as 8 bits, and 8 bits as the channel's value: the
       blend's divisions, done once per output format */
    std::vector<uint8_t> to8_[3];
    std::vector<uint32_t> from8_[3];
    std::vector<Span> covered_;      /* what the overlay drawn in the buffer covers */
    std::vector<uint8_t> under_;     /* the pixels it covers, in that order */
    bool drawn_ = false;             /* the buffer holds the overlay now */
    Key presented_;                  /* what the screen was last shown */
    std::vector<uint16_t> full_;     /* the line runs of a whole frame */
    uint64_t frames_ = 0;
    /* the arrow's glide: from where it was drawn to the pointer, since when.
       Mutable because asking where the arrow is is what moves it on. */
    struct Glide {
        bool shown = false;
        float from_x = 0, from_y = 0, to_x = 0, to_y = 0;
        uint32_t since_ms = 0;
    };
    mutable Glide glide_;
};

Overlay &Instance();

} /* namespace presence */

/* The debugger's PRESENCE command: the reply lines, or false and the error. */
bool PRESENCE_Command(const std::string &args, std::vector<std::string> &lines);
/* The render's output changed size or format: `bytes` per pixel, 0 for one it
   cannot be drawn on. */
void PRESENCE_OutputChanged(unsigned width, unsigned height, unsigned bytes);
/* The scaler is about to write the output buffer `buf`: see Overlay::Undraw. */
void PRESENCE_BeginFrame(uint8_t *buf, size_t pitch);
/* The frame about to be presented: see Overlay::Compose. */
const uint16_t *PRESENCE_EndFrame(uint8_t *buf, size_t pitch, const uint16_t *changed,
                                  bool shown = true);
/* Whether a frame the scaler did not write should still be presented. */
bool PRESENCE_WantsRepaint();
/* The debugger holds the machine: repaint if the overlay moved (a fade, a new state). */
void PRESENCE_ParkedTick();

#endif
