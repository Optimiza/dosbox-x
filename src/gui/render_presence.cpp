/*
 *  Presence indicator: see include/render_presence.h and README.debugger.
 *
 *  The overlay is drawn into the scaled output buffer after the frame was
 *  handed to capture, so nothing that records the emulated picture sees it.
 *  The scaler writes that same buffer in place and rewrites only the blocks of
 *  a line that changed, so a line it is about to write first gets the picture
 *  back from the clean copy (UndrawRows), and a row it rewrote is drawn whole
 *  again; blending onto the blocks it left alone would blend the overlay onto
 *  itself. Every other row keeps the overlay drawn last frame and is touched
 *  only where a layer changed, the way Keen restored only its dirty tiles.
 *
 *  The layers are composited once per change into a premultiplied ARGB buffer
 *  the size of the output, so a frame blends one ready pixel onto the picture
 *  and never asks the look again. An output with a texture of its own for the
 *  overlay (OpenGL) takes that buffer as it is, uploads only its changed
 *  rectangles, and the picture is never read or written at all: GLQuake's 2D
 *  over the 3D view, a quad that costs the GPU a blend and the CPU nothing.
 */

#include "render_presence.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <locale>
#include <sstream>

#include "dosbox.h"
#include "mouse.h"
#include "render.h"
#include "timer.h"
#include "video.h"

/* the VGA ROM fonts (src/ints/int10.h, which needs the memory headers) */
extern uint8_t int10_font_08[256 * 8];
extern uint8_t int10_font_16[256 * 16];
extern unsigned char *scalerSourceCacheBuffer;
extern bool useTraditionalRenderCache;

namespace presence {

namespace {

/* Alphas are quantized to this many levels, so a fade repaints a bounded number of times. */
constexpr int ALPHA_LEVELS = 64;
constexpr int CAPTION_LEVELS = 16;
constexpr int MAX_SPRITE_SIDE = 64;
constexpr size_t MAX_CAPTION = 160, MAX_KEYS = 8, MAX_KEY = 12;
/* The band's layout, in guest pixels: a glyph's width, the padding inside a box, the gap
   between boxes, the pause mark's bars, and the blank a scrolling caption leaves before it
   comes round again. */
constexpr unsigned GLYPH_W = 8, PAD = 2, GAP = 2, BAR_W = 3, PAUSE_W = 2 * PAD + 3 * BAR_W;
constexpr unsigned SCROLL_GAP = 4 * GLYPH_W;
/* The record mark: its dot is this many eighths of the font's height, and audio is the VGA
   font's double note. */
constexpr unsigned REC_DOT_EIGHTHS = 5;
constexpr char NOTE_GLYPH = '\x0E';
const char *const LAYER_NAMES[LAYER_COUNT] = {"edge", "pointer", "keys", "calls", "pause", "record"};
const char *const RECORDING_NAMES[] = {"none", "video", "audio", "both"};

unsigned LowestBit(uint32_t v) {
    unsigned n = 0;
    while (!(v & 1u)) { v >>= 1; n++; }
    return n;
}

bool ParseHex(const std::string &s, uint32_t &out) {
    if (s.size() != 6) return false;
    for (const char c : s)
        if (!std::isxdigit((unsigned char)c)) return false;
    out = (uint32_t)std::strtoul(s.c_str(), nullptr, 16);
    return true;
}

/* A decimal number in [lo, hi], read the same whatever the process's locale. */
bool ParseFloat(const std::string &s, double lo, double hi, float &out) {
    if (s.empty() || s.find_first_not_of("0123456789.") != std::string::npos) return false;
    std::istringstream in(s);
    in.imbue(std::locale::classic());
    double v = 0;
    if (!(in >> v) || !in.eof() || !(v >= lo && v <= hi)) return false;
    out = (float)v;
    return true;
}

/* `%XX` for any byte, every other byte printable ASCII and not `%`, so the text is one word. */
bool Decode(const std::string &s, size_t max, std::string &out) {
    out.clear();
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '%') {
            if (i + 2 >= s.size() || !std::isxdigit((unsigned char)s[i + 1]) ||
                !std::isxdigit((unsigned char)s[i + 2]))
                return false;
            out += (char)std::strtoul(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (c > 0x20 && c < 0x7f) {
            out += (char)c;
        } else {
            return false;
        }
        if (out.size() > max) return false;
    }
    return true;
}

std::vector<std::string> Split(const std::string &s) {
    std::vector<std::string> parts;
    if (s.empty()) return parts;
    std::istringstream in(s);
    std::string part;
    while (std::getline(in, part, ',')) parts.push_back(part);
    if (s.back() == ',') parts.push_back("");
    return parts;
}

const char *StateName(State s) {
    return s == State::Acting ? "acting" : s == State::Held ? "held" : s == State::Paused ? "paused" : "off";
}

/* A colour at alpha `a` of 255, premultiplied. */
uint32_t Premul(uint32_t rgb, unsigned a) {
    const uint32_t r = (((rgb >> 16) & 0xff) * a + 127) / 255;
    const uint32_t g = (((rgb >> 8) & 0xff) * a + 127) / 255;
    const uint32_t b = ((rgb & 0xff) * a + 127) / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

unsigned Alpha255(float alpha) {
    return (unsigned)std::lround(std::min(std::max(alpha, 0.0f), 1.0f) * 255.0f);
}

/* What is left of the colour under an alpha of 255, on a 0 to 256 scale so the product is a
   shift: 255 maps to 256 and leaves nothing. */
uint32_t Remain(uint32_t a) { return 256 - (a + (a >> 7)); }

/* Premultiplied `src` over premultiplied `dst`: two channels a multiply (Herf's lanes). */
uint32_t Over(uint32_t dst, uint32_t src) {
    const uint32_t k = Remain(src >> 24);
    const uint32_t rb = ((dst & 0x00FF00FFu) * k >> 8) & 0x00FF00FFu;
    const uint32_t ag = (((dst >> 8) & 0x00FF00FFu) * k) & 0xFF00FF00u;
    return src + rb + ag;
}

} /* namespace */

namespace placement {

void Points(unsigned width, unsigned height, Point top[SAMPLES], Point bottom[SAMPLES]) {
    const unsigned band = std::max(1u, height / BAND_DIVISOR);
    for (unsigned j = 0; j < ROWS; j++)
        for (unsigned i = 0; i < COLUMNS; i++) {
            const unsigned x = (2 * i + 1) * width / (2 * COLUMNS);
            const unsigned y = (2 * j + 1) * band / (2 * ROWS);
            top[j * COLUMNS + i] = {x, y};
            bottom[j * COLUMNS + i] = {x, height - band + y};
        }
}

unsigned Score(const uint32_t colours[SAMPLES]) {
    unsigned most = 0;
    for (unsigned i = 0; i < SAMPLES; i++) {
        unsigned n = 0;
        for (unsigned j = 0; j < SAMPLES; j++) n += colours[j] == colours[i];
        most = std::max(most, n);
    }
    return SAMPLES - most;
}

Place Step(Place band, unsigned &streak, unsigned top, unsigned bottom) {
    const int here = (int)(band == Place::Top ? top : bottom);
    const int other = (int)(band == Place::Top ? bottom : top);
    if (here - other < (int)MARGIN) { streak = 0; return band; }
    if (streak + 1 < CONSECUTIVE) { streak++; return band; }
    streak = 0;
    return band == Place::Top ? Place::Bottom : Place::Top;
}

} /* namespace placement */

bool FormatFromPixels(unsigned bytes, uint32_t black, uint32_t r, uint32_t g, uint32_t b,
                      Format &out) {
    if (bytes != 2 && bytes != 4) return false;
    const uint32_t masks[3] = {r ^ black, g ^ black, b ^ black};
    if ((masks[0] & masks[1]) || (masks[0] & masks[2]) || (masks[1] & masks[2])) return false;
    Format f;
    f.bytes = bytes;
    f.base = black;
    for (int i = 0; i < 3; i++) {
        const uint32_t m = masks[i];
        if (m == 0) return false;
        const unsigned shift = LowestBit(m);
        const uint32_t run = m >> shift;
        if ((run & (run + 1)) != 0) return false;     /* not one contiguous run of bits */
        f.mask[i] = m;
        f.shift[i] = shift;
        f.max[i] = run;
    }
    out = f;
    return true;
}

bool Overlay::LayerKey::operator==(const LayerKey &o) const {
    if (on != o.on) return false;
    if (!on) return true;
    if (n != o.n || stamp != o.stamp) return false;
    for (unsigned i = 0; i < n; i++)
        if (!(r[i] == o.r[i])) return false;
    return true;
}

bool Overlay::Configure(const std::string &args, std::string &error, uint32_t now_ms) {
    std::istringstream in(args);
    std::string word;
    if (!(in >> word)) { error = "PRESENCE needs a state: off, held, acting or paused"; return false; }
    State next;
    if (word == "off") next = State::Off;
    else if (word == "held") next = State::Held;
    else if (word == "acting") next = State::Acting;
    else if (word == "paused") next = State::Paused;
    else { error = "PRESENCE state must be off, held, acting or paused, not " + word; return false; }

    Look look = look_;
    bool pointer = pointer_;
    bool layers[LAYER_COUNT];
    std::copy(layers_, layers_ + LAYER_COUNT, layers);
    Place place = place_;
    uint32_t tint = tint_;
    Recording recording = recording_;
    bool have_caption = false, have_serial = false, have_keys = false;
    std::string caption;
    uint64_t serial = 0;
    std::vector<std::string> keys;
    while (in >> word) {
        const size_t eq = word.find('=');
        if (eq == std::string::npos) { error = "PRESENCE argument without a value: " + word; return false; }
        const std::string key = word.substr(0, eq), value = word.substr(eq + 1);
        bool ok = true;
        if (key == "rgb") ok = ParseHex(value, look.rgb);
        else if (key == "outline") ok = ParseHex(value, look.outline);
        else if (key == "edge") ok = ParseFloat(value, 0.001, 0.25, look.edge);
        else if (key == "acting") ok = ParseFloat(value, 0.0f, 1.0f, look.acting);
        else if (key == "held") ok = ParseFloat(value, 0.0f, 1.0f, look.held);
        else if (key == "fade") ok = ParseFloat(value, 0.0f, 60.0f, look.fade);
        else if (key == "glide") ok = ParseFloat(value, 0.0f, 5.0f, look.glide);
        else if (key == "caption_text_rgb") ok = ParseHex(value, look.caption_text_rgb);
        else if (key == "caption_alpha") ok = ParseFloat(value, 0.0f, 1.0f, look.caption_alpha);
        else if (key == "scroll_cps") ok = ParseFloat(value, 0.0f, 100.0f, look.scroll_cps);
        else if (key == "caption_in") ok = ParseFloat(value, 0.0f, 5.0f, look.caption_in);
        else if (key == "caption_out") ok = ParseFloat(value, 0.0f, 5.0f, look.caption_out);
        else if (key == "keycap_rgb") ok = ParseHex(value, look.keycap_rgb);
        else if (key == "keycap_text_rgb") ok = ParseHex(value, look.keycap_text_rgb);
        else if (key == "keycap_alpha") ok = ParseFloat(value, 0.0f, 1.0f, look.keycap_alpha);
        else if (key == "record_rgb") ok = ParseHex(value, look.record_rgb);
        else if (key == "record_blink_s") ok = ParseFloat(value, 0.1, 10.0, look.record_blink_s);
        else if (key == "tint") ok = ParseHex(value, tint);
        else if (key == "rec") {
            const auto *at = std::find_if(std::begin(RECORDING_NAMES), std::end(RECORDING_NAMES),
                                          [&](const char *n) { return value == n; });
            ok = at != std::end(RECORDING_NAMES);
            if (ok) recording = (Recording)(at - std::begin(RECORDING_NAMES));
        }
        else if (key == "steps") {
            float steps = 0;
            ok = ParseFloat(value, 1.0f, 64.0f, steps) && steps == std::floor(steps);
            look.steps = (int)steps;
        }
        else if (key == "pointer") {
            ok = value == "0" || value == "1";
            pointer = value == "1";
        }
        else if (key == "sprite") {
            std::vector<std::string> rows;
            std::istringstream rin(value);
            std::string row;
            while (std::getline(rin, row, '/')) rows.push_back(row);
            ok = !rows.empty() && rows.size() <= (size_t)MAX_SPRITE_SIDE;
            for (const auto &r : rows)
                ok = ok && r.size() == rows[0].size() && !r.empty() && r.size() <= (size_t)MAX_SPRITE_SIDE
                     && r.find_first_not_of("Xo.") == std::string::npos;
            if (ok) look.sprite = rows;
        }
        else if (key == "layers") {
            std::fill(layers, layers + LAYER_COUNT, false);
            for (const std::string &name : Split(value)) {
                const auto *at = std::find_if(LAYER_NAMES, LAYER_NAMES + LAYER_COUNT,
                                              [&](const char *n) { return name == n; });
                ok = ok && at != LAYER_NAMES + LAYER_COUNT;
                if (ok) layers[at - LAYER_NAMES] = true;
            }
        }
        else if (key == "place") {
            ok = value == "top" || value == "bottom" || value == "auto";
            place = value == "top" ? Place::Top : value == "bottom" ? Place::Bottom : Place::Auto;
        }
        else if (key == "caption") {
            have_caption = true;
            ok = Decode(value, MAX_CAPTION, caption);
        }
        else if (key == "serial") {
            have_serial = true;
            ok = !value.empty() && value.size() <= 19 &&
                 value.find_first_not_of("0123456789") == std::string::npos;
            if (ok) serial = std::strtoull(value.c_str(), nullptr, 10);
        }
        else if (key == "keys") {
            have_keys = true;
            const std::vector<std::string> names = Split(value);
            ok = names.size() <= MAX_KEYS;
            for (const std::string &name : names) {
                std::string decoded;
                ok = ok && Decode(name, MAX_KEY, decoded) && !decoded.empty();
                keys.push_back(decoded);
            }
        }
        else { error = "PRESENCE does not know the argument " + key; return false; }
        if (!ok) { error = "PRESENCE " + key + "=" + value + " is not a value it takes"; return false; }
    }

    if (next != state_) {
        faded_from_ = state_;
        since_ms_ = now_ms;
    }
    state_ = next;
    /* a client sends the whole look with every state, so only a look that
       differs is a new one, repainted whole */
    if (!(look == look_)) look_generation_++;
    look_ = look;
    if (!pointer) glide_ = Glide();  /* turned off: the next arrow appears where it is */
    pointer_ = pointer;
    std::copy(layers, layers + LAYER_COUNT, layers_);
    if (place != place_) streak_ = 0;
    place_ = place;
    if (tint != tint_) caption_generation_++;
    tint_ = tint;
    if (have_caption) {
        if (caption.empty()) {
            if (caption_on_ && !caption_leaving_) {
                caption_leaving_ = true;
                caption_left_ms_ = now_ms;
            }
        } else {
            const bool same = have_serial ? serial_known_ && serial == serial_ && caption_on_ : false;
            if (!same) {
                caption_on_ = true;
                caption_leaving_ = false;
                caption_since_ms_ = now_ms;
            }
            if (!same || caption != caption_) caption_generation_++;
            caption_ = caption;
        }
    }
    if (have_serial) {
        serial_known_ = true;
        serial_ = serial;
    }
    if (recording != recording_) {
        recording_ = recording;
        recording_since_ms_ = now_ms;
    }
    if (have_keys && keys != keys_) {
        keys_ = keys;
        keys_generation_++;
    }
    return true;
}

void Overlay::Resize(unsigned width, unsigned height, const Format *fmt, bool columns,
                     Texture *texture) {
    width_ = width;
    height_ = height;
    fmt_ = fmt ? *fmt : Format();
    columns_ = columns && texture == nullptr;
    texture_ = texture;
    allocate_ = true;
    uploads_.clear();
    rgb32_ = fmt_.bytes == 4 && fmt_.mask[0] == 0xFF0000u && fmt_.mask[1] == 0xFF00u &&
             fmt_.mask[2] == 0xFFu;
    for (int i = 0; i < 3; i++) {
        to8_[i].clear();
        from8_[i].clear();
        if (fmt_.max[i] == 0) continue;         /* no format: nothing to draw, nothing to convert */
        to8_[i].assign(fmt_.max[i] + 1, 0);
        for (uint32_t v = 0; v <= fmt_.max[i]; v++) to8_[i][v] = (uint8_t)((v * 255) / fmt_.max[i]);
        from8_[i].assign(256, 0);
        for (uint32_t v = 0; v < 256; v++)
            from8_[i][v] = ((v * fmt_.max[i] + 127) / 255) << fmt_.shift[i];
    }
    const size_t pixels = drawable() ? (size_t)width * height : 0;
    over_.assign(pixels, 0);
    /* the texture path never touches the frame, so it keeps no clean copy of it */
    under_.assign(texture_ ? 0 : pixels, 0);
    row_drawn_.assign(drawable() && !texture_ ? height : 0, 1);  /* a new buffer holds no overlay: nothing to take off */
    marks_.assign(drawable() ? height : 0, 0);
    drawn_ = Scene();
    cover_old_.clear();
    pending_.clear();
    present_.clear();
    present_rows_ = nullptr;
    glide_ = Glide();
    cost_ = Cost();
    Rescale();
}

void Overlay::Guest(unsigned width, unsigned height) {
    if (width == guest_width_ && height == guest_height_) return;
    guest_width_ = width;
    guest_height_ = height;
    Rescale();
    look_generation_++;              /* every layer is laid out in guest pixels: repaint them */
}

void Overlay::Rescale() {
    scale_x_ = guest_width_ ? (float)width_ / (float)guest_width_ : 1.0f;
    scale_y_ = guest_height_ ? (float)height_ / (float)guest_height_ : 1.0f;
    const unsigned gw = guest_width_ ? guest_width_ : width_, gh = guest_height_ ? guest_height_ : height_;
    gx_of_x_.resize(width_);
    for (unsigned x = 0; x < width_; x++) gx_of_x_[x] = (unsigned)((uint64_t)x * gw / width_);
    gy_of_y_.resize(height_);
    for (unsigned y = 0; y < height_; y++) gy_of_y_[y] = (unsigned)((uint64_t)y * gh / height_);
}

int Overlay::EdgeBand() const {
    const float side = (float)std::min(width_, height_);
    return std::max(1, (int)std::lround(look_.edge * side));
}

/* The pointer's fraction snapped to the top-left of the guest pixel it is in: the guest's cursor
   sits on a whole guest pixel, while the driver's range can be finer (mode 13h halves its x). */
float Overlay::GuestPixel(float fraction, unsigned guest_pixels) {
    fraction = std::min(std::max(fraction, 0.0f), 1.0f);
    if (guest_pixels == 0) return fraction;
    const float cell = std::min(std::floor(fraction * (float)guest_pixels), (float)(guest_pixels - 1));
    return cell / (float)guest_pixels;
}

Rect Overlay::OutRect(unsigned gx0, unsigned gy0, unsigned gx1, unsigned gy1) const {
    const uint64_t gw = guest_width_ ? guest_width_ : width_, gh = guest_height_ ? guest_height_ : height_;
    auto at = [](uint64_t g, uint64_t guest, uint64_t out) {
        return (unsigned)std::min<uint64_t>(out, (g * out + guest - 1) / guest);
    };
    Rect r;
    r.x0 = at(gx0, gw, width_);
    r.x1 = at(gx1, gw, width_);
    r.y0 = at(gy0, gh, height_);
    r.y1 = at(gy1, gh, height_);
    return r;
}

void Overlay::Band(Scene &s, uint32_t now_ms) const {
    const unsigned gw = guest_width_ ? guest_width_ : width_, gh = guest_height_ ? guest_height_ : height_;
    const unsigned bh = std::max(1u, gh / placement::BAND_DIVISOR);
    s.font_h = bh >= 16 + 2 * PAD ? 16 : 8;
    s.box_h = s.font_h + 2 * PAD;
    s.top = place_ == Place::Top || (place_ == Place::Auto && band_ == Place::Top);
    if (s.top) s.box_y = bh > s.box_h ? (bh - s.box_h) / 2 : 0;
    else s.box_y = bh > s.box_h ? gh - bh + (bh - s.box_h) / 2 : gh > s.box_h ? gh - s.box_h : 0;
    const unsigned edge = (unsigned)(((uint64_t)EdgeBand() * gw + width_ - 1) / std::max(1u, width_));
    const unsigned margin = edge + 2;
    unsigned left = margin, right = gw > margin ? gw - margin : 0;
    const unsigned y0 = s.box_y, y1 = s.box_y + s.box_h;
    auto set = [&](Layer l, unsigned gx0, unsigned gx1, uint64_t stamp) {
        LayerKey &k = s.key[l];
        k.r[0] = OutRect(gx0, y0, gx1, y1);
        k.n = 1;
        k.on = !k.r[0].empty();
        k.stamp = stamp;
    };

    /* the record mark keeps the top right corner whatever the band; a band at the top makes
       room for it in both halves of the blink, so the captions never move with it */
    if (layers_[RECORD] && recording_ != Recording::None) {
        const unsigned dot = s.font_h * REC_DOT_EIGHTHS / 8;
        const bool video = recording_ != Recording::Audio, audio = recording_ != Recording::Video;
        const unsigned w = 2 * PAD + (video ? dot + PAD + 3 * GLYPH_W : 0) +
                           (audio ? dot + PAD + GLYPH_W : 0) + (video && audio ? GAP : 0);
        const unsigned edge_y = (unsigned)(((uint64_t)EdgeBand() * gh + height_ - 1) / std::max(1u, height_));
        const unsigned top = edge_y + 2;
        if (right >= left + w && top + s.box_h <= gh) {
            s.rec_x0 = right - w;
            s.rec_y0 = top;
            if (s.top && y0 < top + s.box_h && y1 > top)
                right = s.rec_x0 > left + GAP ? s.rec_x0 - GAP : left;
            const uint32_t period = std::max<uint32_t>(2, (uint32_t)std::lround(look_.record_blink_s * 1000.0f));
            if ((uint32_t)(now_ms - recording_since_ms_) % period < period / 2) {
                LayerKey &k = s.key[RECORD];
                k.r[0] = OutRect(s.rec_x0, top, s.rec_x0 + w, top + s.box_h);
                k.n = 1;
                k.on = !k.r[0].empty();
                k.stamp = ((uint64_t)recording_ << 32) | look_generation_;
            }
        }
    }

    if (layers_[PAUSE] && s.state == State::Paused && left + PAUSE_W <= right) {
        s.pause_x0 = left;
        set(PAUSE, left, left + PAUSE_W, look_generation_);
        left += PAUSE_W + GAP;
    }

    if (layers_[KEYS] && !keys_.empty()) {
        /* the last keys down are the ones kept when the band has no room for all */
        unsigned x = right;
        size_t shown = 0;
        for (size_t i = keys_.size(); i-- > 0;) {
            const unsigned need = (unsigned)keys_[i].size() * GLYPH_W + 2 * PAD + (shown ? GAP : 0);
            if (x < left + need) break;
            x -= need;
            shown++;
        }
        if (shown) {
            for (size_t i = keys_.size() - shown; i < keys_.size(); i++) {
                s.key_x0.push_back(x);
                x += (unsigned)keys_[i].size() * GLYPH_W + 2 * PAD + GAP;
            }
            set(KEYS, s.key_x0.front(), right, ((uint64_t)keys_generation_ << 32) | (look_generation_ & 0xFFFFu));
            right = s.key_x0.front() > left + GAP ? s.key_x0.front() - GAP : left;
        }
    }

    if (layers_[CALLS] && caption_on_ && right > left + 2 * PAD) {
        float f_in = 1.0f, f_out = 1.0f;
        if (look_.caption_in > 0)
            f_in = std::min(1.0f, (float)(uint32_t)(now_ms - caption_since_ms_) / (look_.caption_in * 1000.0f));
        if (caption_leaving_)
            f_out = look_.caption_out > 0
                ? std::max(0.0f, 1.0f - (float)(uint32_t)(now_ms - caption_left_ms_) / (look_.caption_out * 1000.0f))
                : 0.0f;
        s.cap_level = (int)std::lround(f_in * f_out * CAPTION_LEVELS);
        if (s.cap_level > 0) {
            const unsigned room = right - left, text = (unsigned)caption_.size() * GLYPH_W;
            s.cap_scrolls = text + 2 * PAD > room;
            if (s.cap_scrolls) {
                s.cap_x0 = left;
                s.cap_x1 = right;
                const double moved = (double)(uint32_t)(now_ms - caption_since_ms_) / 1000.0 *
                                     (double)look_.scroll_cps * GLYPH_W;
                /* floored first: 32-bit MSVC's double to uint64_t conversion rounds to nearest,
                   so a step would land half a pixel early */
                s.cap_offset = (unsigned)((uint64_t)std::floor(moved) % (text + SCROLL_GAP));
            } else {
                s.cap_x0 = left + (room - text - 2 * PAD) / 2;
                s.cap_x1 = s.cap_x0 + text + 2 * PAD;
            }
            set(CALLS, s.cap_x0, s.cap_x1,
                ((uint64_t)caption_generation_ << 40) | ((uint64_t)(s.cap_offset & 0xFFFFu) << 24) |
                ((uint64_t)s.cap_level << 16) | (look_generation_ & 0xFFFFu));
        }
    }
}

Overlay::Scene Overlay::SceneAt(uint32_t now_ms, bool have_pointer, float fx, float fy) const {
    Scene s;
    s.state = state_;
    if (state_ == State::Off) return s;
    float alpha = state_ == State::Acting ? look_.acting : look_.held;
    if (state_ != State::Acting && faded_from_ == State::Acting && look_.fade > 0) {
        const float t = (float)(uint32_t)(now_ms - since_ms_) / 1000.0f;
        if (t < look_.fade) alpha = look_.held + (look_.acting - look_.held) * (1.0f - t / look_.fade);
    }
    s.alpha = (int)std::lround(alpha * ALPHA_LEVELS);

    const unsigned w = width_, h = height_, band = std::min((unsigned)EdgeBand(), std::min(w, h));
    if (layers_[EDGE] && s.alpha > 0) {
        LayerKey &k = s.key[EDGE];
        k.on = true;
        k.stamp = ((uint64_t)look_generation_ << 32) | (uint64_t)s.alpha;
        if (2 * band >= h) {
            k.r[0] = {0, 0, w, h};
            k.n = 1;
        } else {
            k.r[0] = {0, 0, w, band};
            k.r[1] = {0, h - band, w, h};
            k.r[2] = {0, band, band, h - band};
            k.r[3] = {w > band ? w - band : 0, band, w, h - band};
            k.n = 4;
        }
    }

    if (layers_[POINTER] && pointer_ && have_pointer && !look_.sprite.empty()) {
        float x = 0, y = 0;
        ArrowAt(now_ms, GuestPixel(fx, guest_width_), GuestPixel(fy, guest_height_), x, y);
        s.ax = (int)std::lround(x * (float)w);
        s.ay = (int)std::lround(y * (float)h);
        const unsigned rows = (unsigned)CellEdge(scale_y_, look_.sprite.size());
        const unsigned cols = (unsigned)CellEdge(scale_x_, look_.sprite[0].size());
        LayerKey &k = s.key[POINTER];
        k.r[0] = {(unsigned)s.ax, (unsigned)s.ay, std::min(w, (unsigned)s.ax + cols),
                  std::min(h, (unsigned)s.ay + rows)};
        k.n = 1;
        /* a scale so small the arrow rounds to nothing draws nothing */
        k.on = !k.r[0].empty();
        k.stamp = look_generation_;
    }

    Band(s, now_ms);
    return s;
}

void Overlay::ArrowAt(uint32_t now_ms, float fx, float fy, float &x, float &y) const {
    Glide &g = glide_;
    const float span_ms = look_.glide * 1000.0f;
    auto at = [&](float &gx, float &gy) {
        const float u = span_ms > 0 ? std::min(1.0f, (float)(uint32_t)(now_ms - g.since_ms) / span_ms) : 1.0f;
        const float eased = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);
        gx = g.from_x + (g.to_x - g.from_x) * eased;
        gy = g.from_y + (g.to_y - g.from_y) * eased;
    };
    if (!g.shown) {
        g = Glide{true, fx, fy, fx, fy, now_ms};
    } else if (fx != g.to_x || fy != g.to_y) {
        /* a new place: glide on from where the arrow is now, not from where it was going */
        at(g.from_x, g.from_y);
        g.to_x = fx;
        g.to_y = fy;
        g.since_ms = now_ms;
    }
    at(x, y);
}

bool Overlay::drawn() const {
    for (const LayerKey &k : drawn_.key)
        if (k.on) return true;
    return false;
}

bool Overlay::NeedsRepaint(uint32_t now_ms, bool have_pointer, float fx, float fy) const {
    if (!drawable()) return false;
    if (!pending_.empty() || !uploads_.empty()) return true;
    const Scene s = SceneAt(now_ms, have_pointer, fx, fy);
    for (int l = 0; l < LAYER_COUNT; l++)
        if (!(s.key[l] == drawn_.key[l])) return true;
    return false;
}

void Overlay::Cover(const Scene &s, std::vector<Rect> &out) {
    out.clear();
    for (const LayerKey &k : s.key)
        if (k.on) out.insert(out.end(), k.r, k.r + k.n);
}

void Overlay::RowIntervals(unsigned y, const Rect *rects, size_t n, std::vector<Interval> &out) {
    out.clear();
    for (size_t i = 0; i < n; i++)
        if (y >= rects[i].y0 && y < rects[i].y1 && rects[i].x0 < rects[i].x1)
            out.push_back({rects[i].x0, rects[i].x1});
    if (out.size() < 2) return;
    std::sort(out.begin(), out.end(), [](const Interval &a, const Interval &b) { return a.x0 < b.x0; });
    size_t m = 0;
    for (size_t i = 1; i < out.size(); i++) {
        if (out[i].x0 <= out[m].x1) out[m].x1 = std::max(out[m].x1, out[i].x1);
        else out[++m] = out[i];
    }
    out.resize(m + 1);
}

void Overlay::Prepare(const Scene &s) {
    const unsigned band = std::min((unsigned)EdgeBand(), std::min(width_, height_));
    edge_px_.assign(band, 0);
    const unsigned a = (unsigned)s.alpha * 255u / ALPHA_LEVELS;
    for (unsigned d = 0; d < band; d++)
        edge_px_[d] = Premul(look_.rgb, a * (unsigned)(look_.steps - (int)(d * (unsigned)look_.steps / band)) /
                                            (unsigned)look_.steps);
    cell_row_.clear();
    cell_col_.clear();
    if (s.key[POINTER].on) {
        for (size_t i = 0; i < look_.sprite.size(); i++)
            for (int y = CellEdge(scale_y_, i); y < CellEdge(scale_y_, i + 1); y++) cell_row_.push_back((uint8_t)i);
        for (size_t i = 0; i < look_.sprite[0].size(); i++)
            for (int x = CellEdge(scale_x_, i); x < CellEdge(scale_x_, i + 1); x++) cell_col_.push_back((uint8_t)i);
    }
}

void Overlay::PaintText(const Scene &s, unsigned y, unsigned x0, unsigned x1, uint32_t *row,
                        unsigned gx0, unsigned gx1, unsigned gy0, const std::string &text,
                        unsigned offset, unsigned period, uint32_t ink) const {
    const unsigned gy = gy_of_y_[y];
    if (gy < gy0 || gy >= gy0 + s.font_h) return;
    const unsigned glyph_row = gy - gy0;
    const uint8_t *font = s.font_h == 16 ? int10_font_16 : int10_font_08;
    const unsigned text_px = (unsigned)text.size() * GLYPH_W;
    for (unsigned x = x0; x < x1; x++) {
        const unsigned gx = gx_of_x_[x];
        if (gx < gx0 || gx >= gx1) continue;
        unsigned u = gx - gx0 + offset;
        if (period) u %= period;
        if (u >= text_px) continue;
        const uint8_t bits = font[(unsigned char)text[u / GLYPH_W] * s.font_h + glyph_row];
        if (bits & (0x80u >> (u % GLYPH_W))) row[x] = Over(row[x], ink);
    }
}

void Overlay::PaintRow(const Scene &s, unsigned y, unsigned x0, unsigned x1) {
    uint32_t *row = &over_[(size_t)y * width_];
    std::fill(row + x0, row + x1, 0u);
    for (int l = 0; l < LAYER_COUNT; l++) {
        const LayerKey &k = s.key[l];
        if (!k.on) continue;
        for (unsigned i = 0; i < k.n; i++) {
            const Rect &r = k.r[i];
            if (y < r.y0 || y >= r.y1) continue;
            const unsigned a = std::max(x0, r.x0), b = std::min(x1, r.x1);
            if (a >= b) continue;
            switch (l) {
            case EDGE: {
                const unsigned band = (unsigned)edge_px_.size();
                const unsigned dy = std::min(y, height_ - 1 - y);
                for (unsigned x = a; x < b; x++) {
                    const unsigned d = std::min(dy, std::min(x, width_ - 1 - x));
                    if (d < band) row[x] = Over(row[x], edge_px_[d]);
                }
                break;
            }
            case POINTER: {
                const std::string &line = look_.sprite[cell_row_[y - r.y0]];
                const uint32_t fill = Premul(look_.rgb, 255), outline = Premul(look_.outline, 255);
                for (unsigned x = a; x < b; x++) {
                    const char c = line[cell_col_[x - r.x0]];
                    if (c != '.') row[x] = c == 'X' ? outline : fill;
                }
                break;
            }
            case KEYS: {
                const uint32_t cap = Premul(look_.keycap_rgb, Alpha255(look_.keycap_alpha));
                const uint32_t ink = Premul(look_.keycap_text_rgb, 255);
                const size_t first = keys_.size() - s.key_x0.size();
                for (size_t n = 0; n < s.key_x0.size(); n++) {
                    const std::string &name = keys_[first + n];
                    const unsigned gx0 = s.key_x0[n], gx1 = gx0 + (unsigned)name.size() * GLYPH_W + 2 * PAD;
                    for (unsigned x = a; x < b; x++)
                        if (gx_of_x_[x] >= gx0 && gx_of_x_[x] < gx1) row[x] = Over(row[x], cap);
                    PaintText(s, y, a, b, row, gx0 + PAD, gx1 - PAD, s.box_y + PAD, name, 0, 0, ink);
                }
                break;
            }
            case CALLS: {
                const unsigned level = (unsigned)s.cap_level;
                const uint32_t box = Premul(tint_, Alpha255(look_.caption_alpha) * level / CAPTION_LEVELS);
                const uint32_t ink = Premul(look_.caption_text_rgb, 255 * level / CAPTION_LEVELS);
                for (unsigned x = a; x < b; x++) row[x] = Over(row[x], box);
                const unsigned period = s.cap_scrolls ? (unsigned)caption_.size() * GLYPH_W + SCROLL_GAP : 0;
                PaintText(s, y, a, b, row, s.cap_x0 + PAD, s.cap_x1 - PAD, s.box_y + PAD, caption_,
                          s.cap_offset, period, ink);
                break;
            }
            case PAUSE: {
                const uint32_t cap = Premul(look_.keycap_rgb, Alpha255(look_.keycap_alpha));
                const uint32_t ink = Premul(look_.keycap_text_rgb, 255);
                const unsigned gy = gy_of_y_[y];
                const bool bars = gy >= s.box_y + PAD && gy < s.box_y + s.box_h - PAD;
                for (unsigned x = a; x < b; x++) {
                    row[x] = Over(row[x], cap);
                    const unsigned lx = gx_of_x_[x] - s.pause_x0;
                    if (bars && ((lx >= PAD && lx < PAD + BAR_W) || (lx >= PAD + 2 * BAR_W && lx < PAD + 3 * BAR_W)))
                        row[x] = Over(row[x], ink);
                }
                break;
            }
            case RECORD: {
                const uint32_t cap = Premul(look_.keycap_rgb, Alpha255(look_.keycap_alpha));
                const uint32_t ink = Premul(look_.keycap_text_rgb, 255);
                const uint32_t red = Premul(look_.record_rgb, 255);
                const unsigned dot = s.font_h * REC_DOT_EIGHTHS / 8, gy = gy_of_y_[y];
                const unsigned dot_y0 = s.rec_y0 + (s.box_h - dot) / 2;
                for (unsigned x = a; x < b; x++) row[x] = Over(row[x], cap);
                unsigned gx0 = s.rec_x0 + PAD;
                /* a disc: a pixel whose centre lies within the dot's diameter of the cell's */
                auto mark = [&](const std::string &label) {
                    if (gy >= dot_y0 && gy < dot_y0 + dot) {
                        const int uy = 2 * (int)(gy - dot_y0) + 1 - (int)dot;
                        for (unsigned x = a; x < b; x++) {
                            const unsigned gx = gx_of_x_[x];
                            if (gx < gx0 || gx >= gx0 + dot) continue;
                            const int ux = 2 * (int)(gx - gx0) + 1 - (int)dot;
                            if (ux * ux + uy * uy <= (int)(dot * dot)) row[x] = Over(row[x], red);
                        }
                    }
                    const unsigned tx = gx0 + dot + PAD, tw = (unsigned)label.size() * GLYPH_W;
                    PaintText(s, y, a, b, row, tx, tx + tw, s.rec_y0 + PAD, label, 0, 0, ink);
                    gx0 = tx + tw + GAP;
                };
                if (recording_ != Recording::Audio) mark("REC");
                if (recording_ != Recording::Video) mark(std::string(1, NOTE_GLYPH));
                break;
            }
            default: break;
            }
        }
    }
}

uint32_t Overlay::Read(const uint8_t *p) const {
    if (fmt_.bytes == 2) { uint16_t v; std::memcpy(&v, p, 2); return v; }
    uint32_t v; std::memcpy(&v, p, 4); return v;
}

void Overlay::Write(uint8_t *p, uint32_t v) const {
    if (fmt_.bytes == 2) { const uint16_t w = (uint16_t)v; std::memcpy(p, &w, 2); return; }
    std::memcpy(p, &v, 4);
}

uint32_t Overlay::Blend(uint32_t under, uint32_t over) const {
    const uint32_t a = over >> 24;
    if (a == 0) return under;
    const uint32_t k = Remain(a);
    if (rgb32_) {
        const uint32_t rb = ((under & 0x00FF00FFu) * k >> 8) & 0x00FF00FFu;
        const uint32_t g = ((under & 0x0000FF00u) * k >> 8) & 0x0000FF00u;
        return (under & 0xFF000000u) | (rb + (over & 0x00FF00FFu)) | (g + (over & 0x0000FF00u));
    }
    uint32_t v = under & ~(fmt_.mask[0] | fmt_.mask[1] | fmt_.mask[2]);
    for (int i = 0; i < 3; i++) {
        const uint32_t was8 = to8_[i][(under & fmt_.mask[i]) >> fmt_.shift[i]];
        const uint32_t now8 = ((over >> (16 - 8 * i)) & 0xff) + (was8 * k >> 8);
        v |= from8_[i][now8];
    }
    return v;
}

void Overlay::UndrawRows(uint8_t *buf, size_t pitch, unsigned y, unsigned rows) {
    if (!drawable() || buf == nullptr || texture_) return;
    const unsigned bytes = fmt_.bytes;
    for (unsigned row = y; row < std::min(height_, y + rows); row++) {
        if (!row_drawn_[row]) continue;
        row_drawn_[row] = 0;
        RowIntervals(row, cover_old_.data(), cover_old_.size(), iv_a_);
        uint8_t *out = buf + (size_t)row * pitch;
        const uint32_t *under = &under_[(size_t)row * width_];
        for (const Interval &iv : iv_a_) {
            if (bytes == 4) std::memcpy(out + (size_t)iv.x0 * 4, under + iv.x0, (size_t)(iv.x1 - iv.x0) * 4);
            else for (unsigned x = iv.x0; x < iv.x1; x++) Write(out + (size_t)x * bytes, under[x]);
            cost_.written += iv.x1 - iv.x0;
        }
    }
}

const uint16_t *Overlay::Compose(uint8_t *buf, size_t pitch, const uint16_t *changed,
                                 uint32_t now_ms, bool have_pointer, float fx, float fy,
                                 bool shown) {
    if (!drawable() || (buf == nullptr && texture_ == nullptr)) return changed;
    const Scene s = SceneAt(now_ms, have_pointer, fx, fy);
    const unsigned bytes = fmt_.bytes;

    /* what changed: the old and the new places of every layer that is not as drawn */
    std::vector<Rect> &dirty = dirty_;
    dirty.clear();
    for (int l = 0; l < LAYER_COUNT; l++) {
        if (s.key[l] == drawn_.key[l]) continue;
        if (drawn_.key[l].on) dirty.insert(dirty.end(), drawn_.key[l].r, drawn_.key[l].r + drawn_.key[l].n);
        if (s.key[l].on) dirty.insert(dirty.end(), s.key[l].r, s.key[l].r + s.key[l].n);
    }
    unsigned dy0 = height_, dy1 = 0;
    for (const Rect &r : dirty) { dy0 = std::min(dy0, r.y0); dy1 = std::max(dy1, r.y1); }

    if (!dirty.empty()) {
        Prepare(s);
        for (unsigned y = dy0; y < dy1; y++) {
            RowIntervals(y, dirty.data(), dirty.size(), iv_a_);
            for (const Interval &iv : iv_a_) PaintRow(s, y, iv.x0, iv.x1);
        }
    }
    if (texture_) {
        uploads_.insert(uploads_.end(), dirty.begin(), dirty.end());
        drawn_ = s;
        if (s.state != State::Off) frames_++;
        return changed;
    }
    std::vector<Rect> &cover = cover_now_;
    Cover(s, cover);

    /* rows still holding the overlay: only where a layer changed. A pixel leaving the
       coverage gets the picture back, one entering it is saved, and the blend reads the
       clean copy, never the output. */
    for (unsigned y = dy0; y < dy1; y++) {
        if (!row_drawn_[y]) continue;
        RowIntervals(y, dirty.data(), dirty.size(), iv_a_);
        RowIntervals(y, cover_old_.data(), cover_old_.size(), iv_b_);
        RowIntervals(y, cover.data(), cover.size(), iv_c_);
        uint8_t *out = buf + (size_t)y * pitch;
        uint32_t *under = &under_[(size_t)y * width_];
        const uint32_t *over = &over_[(size_t)y * width_];
        size_t io = 0, in = 0;
        for (const Interval &d : iv_a_) {
            for (unsigned x = d.x0; x < d.x1;) {
                while (io < iv_b_.size() && iv_b_[io].x1 <= x) io++;
                while (in < iv_c_.size() && iv_c_[in].x1 <= x) in++;
                const bool was = io < iv_b_.size() && iv_b_[io].x0 <= x;
                const bool now = in < iv_c_.size() && iv_c_[in].x0 <= x;
                unsigned end = d.x1;
                if (io < iv_b_.size()) end = std::min(end, was ? iv_b_[io].x1 : iv_b_[io].x0);
                if (in < iv_c_.size()) end = std::min(end, now ? iv_c_[in].x1 : iv_c_[in].x0);
                if (was || now) {
                    for (unsigned px = x; px < end; px++) {
                        uint8_t *p = out + (size_t)px * bytes;
                        if (!was) { under[px] = Read(p); cost_.read++; }
                        Write(p, now ? Blend(under[px], over[px]) : under[px]);
                    }
                    cost_.written += end - x;
                }
                x = end;
            }
        }
    }

    /* rows the scaler rewrote: they hold the picture alone, so the whole coverage is saved
       and blended */
    for (unsigned y = 0; y < height_; y++) {
        if (row_drawn_[y]) continue;
        row_drawn_[y] = 1;
        RowIntervals(y, cover.data(), cover.size(), iv_c_);
        uint8_t *out = buf + (size_t)y * pitch;
        uint32_t *under = &under_[(size_t)y * width_];
        const uint32_t *over = &over_[(size_t)y * width_];
        for (const Interval &iv : iv_c_) {
            for (unsigned x = iv.x0; x < iv.x1; x++) {
                uint8_t *p = out + (size_t)x * bytes;
                under[x] = Read(p);
                Write(p, Blend(under[x], over[x]));
            }
            cost_.read += iv.x1 - iv.x0;
            cost_.written += iv.x1 - iv.x0;
        }
    }

    cover_old_.swap(cover);
    pending_.insert(pending_.end(), dirty.begin(), dirty.end());
    drawn_ = s;
    if (s.state != State::Off) frames_++;
    if (!shown) return changed;
    if (pending_.empty()) {
        cost_.presented += RunBytes(changed);
        present_rows_ = nullptr;
        return changed;
    }

    /* every row a change lies on, merged with the scaler's, for any output; and for one that
       takes rectangles, the changes off the scaler's rows, coalesced over rows alike */
    std::fill(marks_.begin(), marks_.end(), 0);
    if (changed) {
        unsigned y = 0;
        for (size_t i = 0; y < height_; i++) {
            const unsigned n = std::min((unsigned)changed[i], height_ - y);
            if (i & 1) std::fill(marks_.begin() + y, marks_.begin() + y + n, 1);
            y += n;
        }
    }
    present_.clear();
    Coalesce(pending_, present_, &marks_);
    runs_.clear();
    bool on = false;
    unsigned count = 0;
    for (unsigned y = 0; y < height_; y++) {
        if ((marks_[y] != 0) == on) { count++; continue; }
        runs_.push_back((uint16_t)count);
        on = !on;
        count = 1;
    }
    runs_.push_back((uint16_t)count);
    if (columns_) {
        uint64_t area = 0;
        for (const Rect &r : present_) area += (uint64_t)(r.x1 - r.x0) * (r.y1 - r.y0);
        cost_.presented += RunBytes(changed) + area * bytes;
        present_rows_ = changed;
    } else {
        cost_.presented += RunBytes(runs_.data());
        present_rows_ = nullptr;
    }
    pending_.clear();
    return runs_.data();
}

void Overlay::Coalesce(const std::vector<Rect> &in, std::vector<Rect> &out, std::vector<uint8_t> *marks) {
    unsigned y0 = height_, y1 = 0;
    for (const Rect &r : in) { y0 = std::min(y0, r.y0); y1 = std::max(y1, r.y1); }
    size_t open = 0;                         /* the last `open` rectangles grow down */
    iv_b_.clear();
    for (unsigned y = y0; y < y1; y++) {
        RowIntervals(y, in.data(), in.size(), iv_a_);
        if ((marks && (*marks)[y] == 1) || iv_a_.empty()) { open = 0; iv_b_.clear(); continue; }
        const bool same = iv_a_.size() == iv_b_.size() &&
                          std::equal(iv_a_.begin(), iv_a_.end(), iv_b_.begin(),
                                     [](const Interval &p, const Interval &q) { return p.x0 == q.x0 && p.x1 == q.x1; });
        if (same && open) {
            for (size_t i = out.size() - open; i < out.size(); i++) out[i].y1 = y + 1;
        } else {
            for (const Interval &iv : iv_a_) out.push_back({iv.x0, y, iv.x1, y + 1});
            open = iv_a_.size();
            iv_b_ = iv_a_;
        }
        if (marks) (*marks)[y] = 2;
    }
}

void Overlay::Upload() {
    if (texture_ == nullptr || !drawable() || uploads_.empty()) return;
    if (allocate_) {
        texture_->Allocate(width_, height_);
        allocate_ = false;
        uploads_.assign(1, Rect{0, 0, width_, height_});
    }
    upload_rects_.clear();
    Coalesce(uploads_, upload_rects_, nullptr);
    uploads_.clear();
    for (const Rect &r : upload_rects_) {
        texture_->Upload(over_.data(), width_, r);
        cost_.uploads++;
        cost_.upload_bytes += (uint64_t)(r.x1 - r.x0) * (r.y1 - r.y0) * 4;
    }
}

void Overlay::TextureLost() {
    if (texture_ == nullptr) return;
    allocate_ = true;
    uploads_.clear();
    /* nothing drawn: the texture is allocated with the next change, not uploaded empty */
    if (drawn() && drawable()) uploads_.push_back({0, 0, width_, height_});
}

bool Overlay::Columns(const uint16_t *runs, const uint16_t *&rows, const std::vector<Rect> *&rects) const {
    if (!columns_ || runs == nullptr || runs_.empty() || runs != runs_.data()) return false;
    rows = present_rows_;
    rects = &present_;
    return true;
}

void Overlay::Sample(uint32_t now_ms, const Picture &picture) {
    if (place_ != Place::Auto || picture.pixel == nullptr || picture.width == 0 || picture.height == 0)
        return;
    if (sampled_ && (uint32_t)(now_ms - sampled_ms_) < placement::SAMPLE_MS) return;
    sampled_ = true;
    sampled_ms_ = now_ms;
    placement::Point top[placement::SAMPLES], bottom[placement::SAMPLES];
    placement::Points(picture.width, picture.height, top, bottom);
    uint32_t ct[placement::SAMPLES], cb[placement::SAMPLES];
    for (unsigned i = 0; i < placement::SAMPLES; i++) {
        ct[i] = picture.pixel(picture.context, top[i].x, top[i].y);
        cb[i] = picture.pixel(picture.context, bottom[i].x, bottom[i].y);
    }
    cost_.read += 2 * placement::SAMPLES;
    band_ = placement::Step(band_, streak_, placement::Score(ct), placement::Score(cb));
}

/* Walked as the outputs walk them: unchanged and changed counts in turn until the frame's height. */
uint64_t Overlay::RunBytes(const uint16_t *runs) const {
    if (runs == nullptr) return 0;
    uint64_t rows = 0;
    unsigned y = 0;
    for (size_t i = 0; y < height_; i++) {
        const unsigned n = std::min((unsigned)runs[i], height_ - y);
        if (i & 1) rows += n;
        y += n;
    }
    return rows * width_ * fmt_.bytes;
}

std::string Overlay::Describe(bool with_version) const {
    char line[320];
    /* `none` while there is no output yet, `undrawable` for one it cannot draw
       on (8-bit, TrueType text): a client tells "not yet" from "never here" */
    std::string asked;
    for (int l = 0; l < LAYER_COUNT; l++)
        if (layers_[l]) asked += std::string(asked.empty() ? "" : ",") + LAYER_NAMES[l];
    const char *band = band_ == Place::Top ? "top" : "bottom";
    const char *place = place_ == Place::Top ? "top" : place_ == Place::Bottom ? "bottom" : band;
    if (drawable())
        std::snprintf(line, sizeof(line),
                      "PRESENCE %s frames=%llu pointer=%d output=%ux%ux%u px_read=%llu"
                      " px_written=%llu bytes_presented=%llu uploads=%llu upload_bytes=%llu"
                      " layers=%s place=%s",
                      StateName(state_), (unsigned long long)frames_, pointer_ ? 1 : 0, width_,
                      height_, fmt_.bytes * 8u, (unsigned long long)cost_.read,
                      (unsigned long long)cost_.written, (unsigned long long)cost_.presented,
                      (unsigned long long)cost_.uploads, (unsigned long long)cost_.upload_bytes,
                      asked.c_str(), place);
    else if (width_ != 0 && height_ != 0)
        std::snprintf(line, sizeof(line),
                      "PRESENCE %s frames=%llu pointer=%d output=undrawable layers= place=%s%s%s",
                      StateName(state_), (unsigned long long)frames_, pointer_ ? 1 : 0, place,
                      asked.empty() ? "" : " lacks=", asked.c_str());
    else
        std::snprintf(line, sizeof(line), "PRESENCE %s frames=%llu pointer=%d output=none",
                      StateName(state_), (unsigned long long)frames_, pointer_ ? 1 : 0);
    std::string text = line;
    if (with_version) text += " version=" + std::to_string(PROTOCOL_VERSION);
    return text;
}

Overlay &Instance() {
    static Overlay overlay;
    return overlay;
}

} /* namespace presence */

namespace {

uint8_t *frame_buf = nullptr;
size_t frame_pitch = 0;

/* Whether the overlay has anything to do this frame: the cheap test every frame pays. */
bool Idle() {
    const presence::Overlay &o = presence::Instance();
    return o.state() == presence::State::Off && !o.drawn() && !o.NeedsRepaint(0, false, 0, 0);
}

/* The guest's pixel from the scaler's source cache, which never holds the overlay. */
uint32_t GuestPixel(const void *, unsigned x, unsigned y) {
    const uint8_t *line = scalerSourceCacheBuffer + (size_t)y * render.scale.cachePitch;
    switch (render.src.bpp) {
    case 8: {
        const auto &c = render.pal.rgb[line[x]];
        return ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | c.blue;
    }
    case 15: case 16: {
        uint16_t v;
        std::memcpy(&v, line + (size_t)x * 2, 2);
        if (render.src.bpp == 15)
            return ((uint32_t)((v >> 10) & 31) * 255 / 31) << 16 | ((uint32_t)((v >> 5) & 31) * 255 / 31) << 8 |
                   (uint32_t)(v & 31) * 255 / 31;
        return ((uint32_t)((v >> 11) & 31) * 255 / 31) << 16 | ((uint32_t)((v >> 5) & 63) * 255 / 63) << 8 |
               (uint32_t)(v & 31) * 255 / 31;
    }
    case 32: {
        uint32_t v;
        std::memcpy(&v, line + (size_t)x * 4, 4);
        return v & 0xFFFFFFu;
    }
    default: return 0;
    }
}

/* Placement samples the guest's own picture, once a second; the complex scalers keep theirs
   elsewhere, so `auto` stays where it is with them. */
void Sample(uint32_t now_ms) {
    if (scalerSourceCacheBuffer == nullptr || useTraditionalRenderCache) return;
    presence::Picture picture;
    picture.width = (unsigned)render.src.width;
    picture.height = (unsigned)render.src.height;
    picture.pixel = GuestPixel;
    presence::Instance().Sample(now_ms, picture);
}

} /* namespace */

bool PRESENCE_Command(const std::string &args, std::vector<std::string> &lines) {
    std::string error;
    if (args.find_first_not_of(" \t\r\n") == std::string::npos) {
        lines.assign(1, presence::Instance().Describe(true));
        return true;
    }
    if (!presence::Instance().Configure(args, error, (uint32_t)GetTicks())) {
        lines.assign(1, error);
        return false;
    }
    lines.assign(1, presence::Instance().Describe());
    return true;
}

void PRESENCE_OutputChanged(unsigned width, unsigned height, unsigned bytes, bool columns,
                            presence::Texture *texture) {
    presence::Format fmt;
    const bool ok = bytes != 0 && presence::FormatFromPixels(
            bytes, (uint32_t)GFX_GetRGB(0, 0, 0), (uint32_t)GFX_GetRGB(255, 0, 0),
            (uint32_t)GFX_GetRGB(0, 255, 0), (uint32_t)GFX_GetRGB(0, 0, 255), fmt);
    presence::Instance().Resize(width, height, ok ? &fmt : nullptr, columns, ok ? texture : nullptr);
    frame_buf = nullptr;
}

void PRESENCE_BeginFrame(uint8_t *buf, size_t pitch, bool whole) {
    frame_buf = buf;
    frame_pitch = pitch;
    if (whole) presence::Instance().Undraw(buf, pitch);
}

bool PRESENCE_Drawn() {
    return presence::Instance().in_buffer();
}

void PRESENCE_UndrawRows(unsigned y, unsigned rows) {
    presence::Instance().UndrawRows(frame_buf, frame_pitch, y, rows);
}

const uint16_t *PRESENCE_EndFrame(uint8_t *buf, size_t pitch, const uint16_t *changed,
                                  bool shown) {
    if (Idle()) return changed;
    float fx = 0, fy = 0;
    unsigned guest_width = 0, guest_height = 0;
    const bool pointer = Mouse_PresencePointer(fx, fy, guest_width, guest_height);
    const uint32_t now = (uint32_t)GetTicks();
    presence::Instance().Guest(guest_width, guest_height);
    Sample(now);
    return presence::Instance().Compose(buf, pitch, changed, now, pointer, fx, fy, shown);
}

bool PRESENCE_Columns(const uint16_t *changed, const uint16_t *&rows,
                      const std::vector<presence::Rect> *&rects) {
    return presence::Instance().Columns(changed, rows, rects);
}

bool PRESENCE_WantsRepaint() {
    if (Idle()) return false;
    float fx = 0, fy = 0;
    unsigned guest_width = 0, guest_height = 0;
    const bool pointer = Mouse_PresencePointer(fx, fy, guest_width, guest_height);
    const uint32_t now = (uint32_t)GetTicks();
    presence::Instance().Guest(guest_width, guest_height);
    Sample(now);
    return presence::Instance().NeedsRepaint(now, pointer, fx, fy);
}

void PRESENCE_Repaint() {
    presence::Texture *texture = presence::Instance().texture();
    if (texture) {
        /* the frame stays as it was shown: only the overlay's texture changes */
        PRESENCE_EndFrame(nullptr, 0, nullptr);
        texture->Present();
        return;
    }
    uint8_t *pixels = nullptr;
    Bitu pitch = 0;
    if (!GFX_StartUpdate(pixels, pitch)) return;
    GFX_EndUpdate(PRESENCE_EndFrame(pixels, (size_t)pitch, nullptr));
}

void PRESENCE_ParkedTick() {
    if (PRESENCE_WantsRepaint()) PRESENCE_Repaint();
}

bool PRESENCE_TextureDraw(const presence::Texture *texture) {
    presence::Overlay &o = presence::Instance();
    if (texture == nullptr || o.texture() != texture) return false;
    o.Upload();
    return o.drawn();
}

bool PRESENCE_TexturePending(const presence::Texture *texture) {
    const presence::Overlay &o = presence::Instance();
    return texture != nullptr && o.texture() == texture && o.upload_pending();
}

void PRESENCE_TextureLost(const presence::Texture *texture) {
    presence::Overlay &o = presence::Instance();
    if (texture != nullptr && o.texture() == texture) o.TextureLost();
}
