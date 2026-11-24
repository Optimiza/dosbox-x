/*
 *  Presence indicator: a client driving the emulator through the mcp_server
 *  debugger channel marks the window it drives: a soft edge on the output's
 *  inner sides and, as asked, the guest's mouse pointer, the keys held down, a
 *  caption naming the call, a mark while the session holds the machine and a
 *  blinking mark while it records.
 *
 *  It is drawn into the scaled output buffer just before it is presented, or
 *  on OpenGL and Direct3D 9 as a texture of its own over the frame, after the frame has been
 *  handed to capture: screenshots, video capture and the VGA memory never see
 *  it. See README.debugger, "Presence".
 */

#ifndef DOSBOX_RENDER_PRESENCE_H
#define DOSBOX_RENDER_PRESENCE_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace presence {

/* What the bare PRESENCE command announces, so a client can refuse a build older than it
   expects: below this a build draws fewer of the layers the client asks for. */
constexpr int PROTOCOL_VERSION = 3;

enum class State { Off, Held, Acting, Paused };
/* The band of the picture the captions, keycaps and pause mark go in. */
enum class Place { Top, Bottom, Auto };
/* Drawing order, bottom first; the words are the client's layer names. */
enum Layer { EDGE, POINTER, KEYS, CALLS, PAUSE, RECORD, LAYER_COUNT };
/* What the session records, which the record mark shows. */
enum class Recording { None, Video, Audio, Both };

/* How the overlay is drawn: everything comes from the client, so the emulator
   holds no look of its own. Colours are 0xRRGGBB, alphas and the edge 0 to 1,
   times in seconds. Each sprite cell and each glyph pixel is one guest pixel. */
struct Look {
    uint32_t rgb = 0xFF8C00;
    uint32_t outline = 0x2A1400;
    float edge = 0.025f;      /* edge width, a fraction of the shorter side */
    int steps = 6;            /* bands of the edge, strongest at the rim */
    float acting = 0.85f;
    float held = 0.28f;
    float fade = 2.5f;        /* from acting back to held */
    float glide = 0.1f;       /* the arrow easing out to a new place */
    /* the arrow, top row first: 'X' outline, 'o' fill, '.' untouched */
    std::vector<std::string> sprite;
    uint32_t caption_text_rgb = 0xFFFFFF;
    float caption_alpha = 0.7f;
    float scroll_cps = 12.0f; /* characters a second, for a caption wider than its room */
    float caption_in = 0.15f;
    float caption_out = 0.15f;
    uint32_t keycap_rgb = 0x202020;
    uint32_t keycap_text_rgb = 0xFFFFFF;
    float keycap_alpha = 0.8f;
    float font_share = 1.0f / 24.0f; /* the text's height, a share of the picture's */
    uint32_t record_rgb = 0xFF3B30;   /* the record mark's dot */
    float record_blink_s = 1.0f;      /* the record mark's blink, on for its first half */
    bool operator==(const Look &o) const {
        return rgb == o.rgb && outline == o.outline && edge == o.edge && steps == o.steps &&
               acting == o.acting && held == o.held && fade == o.fade && glide == o.glide &&
               sprite == o.sprite && caption_text_rgb == o.caption_text_rgb &&
               caption_alpha == o.caption_alpha && scroll_cps == o.scroll_cps &&
               caption_in == o.caption_in && caption_out == o.caption_out &&
               keycap_rgb == o.keycap_rgb && keycap_text_rgb == o.keycap_text_rgb &&
               keycap_alpha == o.keycap_alpha && font_share == o.font_share &&
               record_rgb == o.record_rgb && record_blink_s == o.record_blink_s;
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

/* What drawing the overlay has cost, counted since the output was made: the output pixels it read
   (saved, sampled for placement), the output pixels it wrote (put back, blended), the bytes it
   handed on to be presented, and the rectangles uploaded to an overlay texture (none on the CPU
   path, and nothing read, written or presented of the frame on the texture path). Pixels and
   bytes, so the same on any host. */
struct Cost {
    uint64_t read = 0;
    uint64_t written = 0;
    uint64_t presented = 0;
    uint64_t uploads = 0;
    uint64_t upload_bytes = 0;
};

/* [x0, x1) by [y0, y1), in output pixels. */
struct Rect {
    unsigned x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x0 >= x1 || y0 >= y1; }
    bool operator==(const Rect &o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
};

/* An output that draws the overlay as a texture of its own, a second quad over the frame, so
   the frame's pixels are never touched. The overlay hands it what changed and asks it to show. */
class Texture {
public:
    virtual ~Texture() = default;
    /* A texture of `width` by `height` pixels, its content undefined until uploaded. */
    virtual void Allocate(unsigned width, unsigned height) = 0;
    /* Rectangle `r` of `pixels`, premultiplied ARGB as 0xAARRGGBB, `pitch` pixels a row. */
    virtual void Upload(const uint32_t *pixels, size_t pitch, const Rect &r) = 0;
    /* Show the frame as it last stood with the overlay over it, uploading nothing of the frame. */
    virtual void Present() = 0;
};

/* The rules `scry_core.placement` states and its test vectors replay. */
namespace placement {
constexpr unsigned BAND_DIVISOR = 8, COLUMNS = 16, ROWS = 2, MARGIN = 4, CONSECUTIVE = 2;
constexpr uint32_t SAMPLE_MS = 1000;
constexpr unsigned SAMPLES = COLUMNS * ROWS;
struct Point { unsigned x, y; };
/* The top band's and the bottom band's samples, row by row, at their cells' centres. */
void Points(unsigned width, unsigned height, Point top[SAMPLES], Point bottom[SAMPLES]);
/* How many of the samples differ from their most common colour. */
unsigned Score(const uint32_t colours[SAMPLES]);
/* The band after one sample of the two bands' scores; `streak` carries across samples. */
Place Step(Place band, unsigned &streak, unsigned top, unsigned bottom);
} /* namespace placement */

/* The guest's picture, for placement: its size and the colour at (x, y) as 0xRRGGBB. */
struct Picture {
    unsigned width = 0, height = 0;
    uint32_t (*pixel)(const void *context, unsigned x, unsigned y) = nullptr;
    const void *context = nullptr;
};

class Overlay {
public:
    /* "<off|held|acting|paused> [key=value ...]": the state, the look, the layers and what
       they show. Keys left out keep their value; a refused command changes nothing. `now_ms`
       is the host clock the fades are timed on. */
    bool Configure(const std::string &args, std::string &error, uint32_t now_ms);

    /* The output the overlay is drawn on; `fmt` null when it cannot be (an 8-bit or text
       output, no output yet). `columns`: the output presents rectangles, not only rows.
       `texture`: the output draws the overlay as that texture, and the buffer Compose is
       handed is never touched. A new output holds no overlay. */
    void Resize(unsigned width, unsigned height, const Format *fmt, bool columns = false,
                Texture *texture = nullptr);

    /* The guest picture the output is scaled from, in the guest's own pixels
       (0 for none: one output pixel a guest pixel). A new size repaints every layer. */
    void Guest(unsigned width, unsigned height);

    /* The scaler is about to write rows [y, y + rows) of `buf`: the pixels the overlay covers
       there are put back, so those rows hold only the emulated picture. */
    void UndrawRows(uint8_t *buf, size_t pitch, unsigned y, unsigned rows);
    /* Every row, for a frame that rewrites them all. */
    void Undraw(uint8_t *buf, size_t pitch) { UndrawRows(buf, pitch, 0, height_); }

    /* Whether the next Compose would change what is on screen. */
    bool NeedsRepaint(uint32_t now_ms, bool have_pointer, float fx, float fy) const;

    /* Draw onto `buf`, the output buffer holding the frame about to be shown, of which the
       scaler presents the line runs `changed` (its alternating unchanged/changed counts;
       nullptr when it wrote nothing). Rows the scaler rewrote get the whole overlay; the rest
       only the layers that changed. Returns the line runs to present: `changed` when no layer
       changed, else `changed` merged with every row a change lies on. `shown` is false for a
       frame that is drawn on but never presented (an aborted one): its changes are handed on
       with the next frame that is. */
    const uint16_t *Compose(uint8_t *buf, size_t pitch, const uint16_t *changed,
                            uint32_t now_ms, bool have_pointer, float fx, float fy,
                            bool shown = true);

    /* For an output that presents rectangles, handed `runs` by the last shown Compose: the
       scaler's own line runs (nullptr for none) and the changed rectangles off those rows.
       False when `runs` carries everything, as it does when no layer changed. */
    bool Columns(const uint16_t *runs, const uint16_t *&rows, const std::vector<Rect> *&rects) const;

    /* On the texture path: hand the texture every rectangle changed since the last call,
       allocating it first when it is new or was lost. Each rectangle is one upload. */
    void Upload();
    /* The texture lost its content (a device reset, a new context): allocated and uploaded
       whole again before it is next drawn. */
    void TextureLost();
    Texture *texture() const { return texture_; }
    /* Whether a change waits to be uploaded to the texture. */
    bool upload_pending() const { return !uploads_.empty(); }

    /* Placement `auto`: score the guest's two bands once every SAMPLE_MS. */
    void Sample(uint32_t now_ms, const Picture &picture);

    State state() const { return state_; }
    Place place() const { return band_; }
    bool drawn() const;
    /* Whether the output buffer holds the overlay, so the scaler must take it off a line. */
    bool in_buffer() const { return texture_ == nullptr && drawn(); }
    uint64_t frames() const { return frames_; }
    const Cost &cost() const { return cost_; }
    bool drawable() const { return fmt_.bytes != 0 && width_ != 0 && height_ != 0; }
    /* One line for the channel's reply. */
    std::string Describe(bool with_version = false) const;

private:
    /* What one layer covers and what it shows, so a change is found by comparing two. */
    struct LayerKey {
        bool on = false;
        unsigned n = 0;
        Rect r[4];
        uint64_t stamp = 0;
        bool operator==(const LayerKey &o) const;
    };
    /* Everything a frame draws, computed from the state and the clock. */
    struct Scene {
        State state = State::Off;
        int alpha = 0;                        /* the edge's, of ALPHA_LEVELS */
        int ax = 0, ay = 0;                   /* the arrow's top left, output pixels */
        bool top = false;                     /* the band's place */
        unsigned box_y = 0, box_h = 0, font_h = 16;   /* guest pixels */
        unsigned pause_x0 = 0;
        unsigned key_y0 = 0;                  /* the held keys' row, guest pixels, on the edge
                                                 the band leaves free */
        unsigned key_font_h = 8, key_box_h = 12;   /* the marks' glyphs and box, guest pixels */
        unsigned rec_x0 = 0, rec_y0 = 0;      /* the record mark's box, guest pixels */
        unsigned cap_x0 = 0, cap_x1 = 0, cap_offset = 0;
        bool cap_scrolls = false;
        int cap_level = 0;                    /* the caption's fade, of CAPTION_LEVELS */
        std::vector<unsigned> key_x0;         /* each shown keycap's left, guest pixels */
        LayerKey key[LAYER_COUNT];
    };
    struct Interval { unsigned x0, x1; };

    Scene SceneAt(uint32_t now_ms, bool have_pointer, float fx, float fy) const;
    void Band(Scene &s, uint32_t now_ms) const;
    /* Where the arrow is drawn at `now_ms`, as a fraction of the output: gliding
       from where it was drawn to the pointer at (fx, fy), the first time there. */
    void ArrowAt(uint32_t now_ms, float fx, float fy, float &x, float &y) const;
    void Rescale();
    int EdgeBand() const;
    static float GuestPixel(float fraction, unsigned guest_pixels);
    static int CellEdge(float scale, size_t i) { return (int)std::lround(scale * (float)i); }
    /* The guest rectangle [gx0, gx1) by [gy0, gy1) in output pixels. */
    Rect OutRect(unsigned gx0, unsigned gy0, unsigned gx1, unsigned gy1) const;
    /* The merged intervals of row `y` inside `rects`. */
    static void RowIntervals(unsigned y, const Rect *rects, size_t n, std::vector<Interval> &out);
    static void Cover(const Scene &s, std::vector<Rect> &out);
    /* The per-row union of `in`, coalesced over rows alike, appended to `out`; with `marks`,
       a row marked 1 is left out and a covered one is marked 2. */
    void Coalesce(const std::vector<Rect> &in, std::vector<Rect> &out, std::vector<uint8_t> *marks);
    void Prepare(const Scene &s);
    void PaintRow(const Scene &s, unsigned y, unsigned x0, unsigned x1);
    /* `text` in the 8- or 16-row font `font_h`, its glyphs' top row at guest row `gy0`. */
    void PaintText(unsigned y, unsigned x0, unsigned x1, uint32_t *row,
                   unsigned gx0, unsigned gx1, unsigned gy0, unsigned font_h,
                   const std::string &text, unsigned offset, unsigned period,
                   uint32_t ink) const;
    uint32_t Blend(uint32_t under, uint32_t over) const;
    uint32_t Read(const uint8_t *p) const;
    void Write(uint8_t *p, uint32_t v) const;
    uint64_t RunBytes(const uint16_t *runs) const;

    Look look_;
    unsigned look_generation_ = 0;
    State state_ = State::Off;
    bool pointer_ = false;
    bool layers_[LAYER_COUNT] = {true, true, true, true, true, true};
    Place place_ = Place::Auto;
    Place band_ = Place::Bottom;            /* where `auto` put the band */
    unsigned streak_ = 0;
    bool sampled_ = false;
    uint32_t sampled_ms_ = 0;
    uint32_t since_ms_ = 0;                 /* when the state last changed */
    State faded_from_ = State::Off;
    /* the caption: its text, its tint, and when it came or began to leave */
    std::string caption_;
    uint32_t tint_ = 0xFF8C00;
    bool caption_on_ = false, caption_leaving_ = false;
    bool serial_known_ = false;
    uint64_t serial_ = 0;
    uint32_t caption_since_ms_ = 0, caption_left_ms_ = 0;
    unsigned caption_generation_ = 0;
    std::vector<std::string> keys_;
    unsigned keys_generation_ = 0;
    Recording recording_ = Recording::None;
    uint32_t recording_since_ms_ = 0;       /* the blink's phase starts at the last change */

    unsigned width_ = 0, height_ = 0;
    unsigned guest_width_ = 0, guest_height_ = 0;
    float scale_x_ = 1, scale_y_ = 1;       /* output pixels a guest pixel, each way */
    std::vector<unsigned> gx_of_x_, gy_of_y_;
    Format fmt_;
    bool rgb32_ = false;                    /* 0x00RRGGBB: two channels a multiply */
    bool columns_ = false;
    Texture *texture_ = nullptr;
    bool allocate_ = false;                 /* the texture is new or lost: allocate, upload whole */
    std::vector<Rect> uploads_;             /* changed since the texture was last uploaded */
    std::vector<Rect> upload_rects_;
    /* each channel's value as 8 bits, and 8 bits as the channel's value */
    std::vector<uint8_t> to8_[3];
    std::vector<uint32_t> from8_[3];

    std::vector<uint32_t> over_;            /* the layers, premultiplied ARGB, output-sized */
    std::vector<uint32_t> under_;           /* the picture under every covered pixel */
    std::vector<uint8_t> row_drawn_;        /* the row holds the overlay as `drawn_` covers it */
    Scene drawn_;                           /* what over_ and the buffer hold */
    std::vector<Rect> pending_;             /* changed since the screen was last shown */
    std::vector<uint16_t> runs_;            /* the merged runs handed on */
    std::vector<Rect> present_;             /* the rectangles handed on beside them */
    const uint16_t *present_rows_ = nullptr;
    /* per frame, kept to spare allocations */
    std::vector<uint32_t> edge_px_;
    std::vector<uint8_t> cell_row_, cell_col_;
    std::vector<uint8_t> marks_;
    std::vector<Interval> iv_a_, iv_b_, iv_c_;
    std::vector<Rect> cover_old_, cover_now_, dirty_;
    uint64_t frames_ = 0;
    Cost cost_;
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
/* The render's output changed size or format: `bytes` per pixel, 0 for one it cannot be drawn
   on; `columns` when the output presents rectangles (PRESENCE_Columns); `texture` when it draws
   the overlay as a texture of its own. */
void PRESENCE_OutputChanged(unsigned width, unsigned height, unsigned bytes, bool columns,
                            presence::Texture *texture = nullptr);
/* The scaler is about to write the output buffer `buf`; `whole` when it rewrites every row. */
void PRESENCE_BeginFrame(uint8_t *buf, size_t pitch, bool whole);
/* Whether the overlay is in the buffer, so a scaler line has to ask before writing. */
bool PRESENCE_Drawn();
/* The scaler is about to write `rows` rows from `y` of the buffer PRESENCE_BeginFrame named. */
void PRESENCE_UndrawRows(unsigned y, unsigned rows);
/* The frame about to be presented: see Overlay::Compose. */
const uint16_t *PRESENCE_EndFrame(uint8_t *buf, size_t pitch, const uint16_t *changed,
                                  bool shown = true);
/* For an output handed `changed`: see Overlay::Columns. */
bool PRESENCE_Columns(const uint16_t *changed, const uint16_t *&rows,
                      const std::vector<presence::Rect> *&rects);
/* Whether a frame the scaler did not write should still be presented. */
bool PRESENCE_WantsRepaint();
/* Show the overlay as it is now over a frame the scaler did not write. */
void PRESENCE_Repaint();
/* The debugger holds the machine: repaint if the overlay moved (a fade, a new state). */
void PRESENCE_ParkedTick();
/* For the output `texture`: upload what changed, and whether there is an overlay to draw. */
bool PRESENCE_TextureDraw(const presence::Texture *texture);
/* Whether the overlay changed since `texture` was last uploaded, so a frame with no new
   picture is still to be shown. */
bool PRESENCE_TexturePending(const presence::Texture *texture);
/* `texture` lost its content: see Overlay::TextureLost. */
void PRESENCE_TextureLost(const presence::Texture *texture);

#endif
