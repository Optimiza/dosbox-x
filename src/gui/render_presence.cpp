/*
 *  Presence indicator: see include/render_presence.h and README.debugger.
 *
 *  The overlay is drawn into the scaled output buffer after the frame was
 *  handed to capture, so nothing that records the emulated picture sees it.
 *  The scaler writes that same buffer in place and rewrites only the blocks of
 *  a line that changed, so the overlay never stays in it while the scaler
 *  works: the pixels it covers are saved when it is drawn and put back before
 *  the scaler writes again (Undraw), and every frame it is drawn anew over a
 *  buffer that holds only the emulated picture. Blending onto pixels the scaler
 *  left alone would otherwise blend the overlay onto itself.
 */

#include "render_presence.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <locale>
#include <sstream>

#include "dosbox.h"
#include "mouse.h"
#include "timer.h"
#include "video.h"

namespace presence {

namespace {

/* Alphas are quantized to this many levels, so a fade repaints a bounded number of times. */
constexpr int ALPHA_LEVELS = 64;
constexpr int MAX_SPRITE_SIDE = 64;

unsigned LowestBit(uint32_t v) {
    unsigned n = 0;
    while (!(v & 1u)) { v >>= 1; n++; }
    return n;
}

uint32_t ReadPixel(const uint8_t *p, unsigned bytes) {
    if (bytes == 2) { uint16_t v; std::memcpy(&v, p, 2); return v; }
    uint32_t v; std::memcpy(&v, p, 4); return v;
}

void WritePixel(uint8_t *p, unsigned bytes, uint32_t v) {
    if (bytes == 2) { const uint16_t w = (uint16_t)v; std::memcpy(p, &w, 2); return; }
    std::memcpy(p, &v, 4);
}

bool ParseHex(const std::string &s, uint32_t &out) {
    if (s.size() != 6) return false;
    for (const char c : s)
        if (!std::isxdigit((unsigned char)c)) return false;
    out = (uint32_t)std::strtoul(s.c_str(), nullptr, 16);
    return true;
}

/* A decimal number in [lo, hi], read the same whatever the process's locale. */
bool ParseFloat(const std::string &s, float lo, float hi, float &out) {
    if (s.empty() || s.find_first_not_of("0123456789.") != std::string::npos) return false;
    std::istringstream in(s);
    in.imbue(std::locale::classic());
    double v = 0;
    if (!(in >> v) || !in.eof() || !(v >= lo && v <= hi)) return false;
    out = (float)v;
    return true;
}

const char *StateName(State s) {
    return s == State::Acting ? "acting" : s == State::Held ? "held" : "off";
}

} /* namespace */

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

bool Overlay::Configure(const std::string &args, std::string &error, uint32_t now_ms) {
    std::istringstream in(args);
    std::string word;
    if (!(in >> word)) { error = "PRESENCE needs a state: off, held or acting"; return false; }
    State next;
    if (word == "off") next = State::Off;
    else if (word == "held") next = State::Held;
    else if (word == "acting") next = State::Acting;
    else { error = "PRESENCE state must be off, held or acting, not " + word; return false; }

    Look look = look_;
    bool pointer = pointer_;
    while (in >> word) {
        const size_t eq = word.find('=');
        if (eq == std::string::npos) { error = "PRESENCE argument without a value: " + word; return false; }
        const std::string key = word.substr(0, eq), value = word.substr(eq + 1);
        bool ok = true;
        if (key == "rgb") ok = ParseHex(value, look.rgb);
        else if (key == "outline") ok = ParseHex(value, look.outline);
        else if (key == "edge") ok = ParseFloat(value, 0.001f, 0.25f, look.edge);
        else if (key == "acting") ok = ParseFloat(value, 0.0f, 1.0f, look.acting);
        else if (key == "held") ok = ParseFloat(value, 0.0f, 1.0f, look.held);
        else if (key == "fade") ok = ParseFloat(value, 0.0f, 60.0f, look.fade);
        else if (key == "glide") ok = ParseFloat(value, 0.0f, 5.0f, look.glide);
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
    return true;
}

void Overlay::Resize(unsigned width, unsigned height, const Format *fmt, unsigned guest_width,
                     unsigned guest_height) {
    width_ = width;
    height_ = height;
    scale_x_ = guest_width ? (float)width / (float)guest_width : 1.0f;
    scale_y_ = guest_height ? (float)height / (float)guest_height : 1.0f;
    fmt_ = fmt ? *fmt : Format();
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
    covered_.clear();
    under_.clear();
    drawn_ = false;                  /* a new output buffer holds no overlay */
    presented_ = Key();
    glide_ = Glide();
    full_.assign({0, (uint16_t)std::min(height, 65535u)});
}

int Overlay::Band() const {
    const float side = (float)std::min(width_, height_);
    return std::max(1, (int)std::lround(look_.edge * side));
}

Overlay::Key Overlay::KeyAt(uint32_t now_ms, bool have_pointer, float fx, float fy) const {
    Key k;
    k.state = state_;
    if (state_ == State::Off) return k;
    float alpha = state_ == State::Acting ? look_.acting : look_.held;
    if (state_ == State::Held && faded_from_ == State::Acting && look_.fade > 0) {
        const float t = (float)(uint32_t)(now_ms - since_ms_) / 1000.0f;
        if (t < look_.fade) alpha = look_.held + (look_.acting - look_.held) * (1.0f - t / look_.fade);
    }
    k.alpha = (int)std::lround(alpha * ALPHA_LEVELS);
    k.look = look_generation_;
    k.arrow = pointer_ && have_pointer && !look_.sprite.empty();
    if (k.arrow) {
        float x = 0, y = 0;
        ArrowAt(now_ms, std::min(std::max(fx, 0.0f), 1.0f), std::min(std::max(fy, 0.0f), 1.0f), x, y);
        k.ax = (int)std::lround(x * (float)width_);
        k.ay = (int)std::lround(y * (float)height_);
    }
    return k;
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

bool Overlay::NeedsRepaint(uint32_t now_ms, bool have_pointer, float fx, float fy) const {
    return drawable() && !(KeyAt(now_ms, have_pointer, fx, fy) == presented_);
}

std::vector<Overlay::Span> Overlay::Covered(const Key &key) const {
    std::vector<Span> spans;
    if (key.state == State::Off) return spans;
    const unsigned w = width_, h = height_, band = (unsigned)Band();
    for (unsigned y = 0; y < h; y++) {
        if (y < band || y + band >= h) spans.push_back({y, 0, w});
        else {
            spans.push_back({y, 0, std::min(band, w)});
            spans.push_back({y, w > band ? w - band : 0, w});
        }
    }
    if (key.arrow) {
        const unsigned rows = (unsigned)CellEdge(scale_y_, look_.sprite.size());
        const unsigned cols = (unsigned)CellEdge(scale_x_, look_.sprite[0].size());
        for (unsigned y = (unsigned)key.ay; y < std::min(h, (unsigned)key.ay + rows); y++)
            if ((unsigned)key.ax < w) spans.push_back({y, (unsigned)key.ax, std::min(w, (unsigned)key.ax + cols)});
    }
    return spans;
}

uint32_t Overlay::Pixel(uint32_t rgb) const {
    const uint32_t c8[3] = {(rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff};
    uint32_t v = fmt_.base;
    for (int i = 0; i < 3; i++)
        v = (v & ~fmt_.mask[i]) | (((c8[i] * fmt_.max[i] + 127) / 255) << fmt_.shift[i]);
    return v;
}

uint32_t Overlay::Blend(uint32_t under, const uint32_t c8[3], int alpha256) const {
    uint32_t v = under & ~(fmt_.mask[0] | fmt_.mask[1] | fmt_.mask[2]);
    for (int i = 0; i < 3; i++) {
        const uint32_t was8 = to8_[i][(under & fmt_.mask[i]) >> fmt_.shift[i]];
        const uint32_t now8 = (was8 * (uint32_t)(256 - alpha256) + c8[i] * (uint32_t)alpha256) >> 8;
        v |= from8_[i][now8];
    }
    return v;
}

void Overlay::Draw(uint8_t *buf, size_t pitch, const Key &key) const {
    const unsigned bytes = fmt_.bytes;
    const int w = (int)width_, h = (int)height_, band = Band();
    const int a = key.alpha * 256 / ALPHA_LEVELS;
    const uint32_t c8[3] = {(look_.rgb >> 16) & 0xff, (look_.rgb >> 8) & 0xff, look_.rgb & 0xff};
    /* the alpha at each distance from the rim, so a pixel costs no division */
    std::vector<int> alpha_at((size_t)band);
    for (int d = 0; d < band; d++) alpha_at[(size_t)d] = a * (look_.steps - d * look_.steps / band) / look_.steps;
    for (int y = 0; y < h; y++) {
        uint8_t *out = buf + (size_t)y * pitch;
        auto shade = [&](int x) {
            const int d = std::min(std::min(x, w - 1 - x), std::min(y, h - 1 - y));
            uint8_t *p = out + (size_t)x * bytes;
            WritePixel(p, bytes, Blend(ReadPixel(p, bytes), c8, alpha_at[(size_t)d]));
        };
        if (y < band || y >= h - band) {
            for (int x = 0; x < w; x++) shade(x);
        } else {
            for (int x = 0; x < std::min(band, w); x++) shade(x);
            for (int x = std::max(w - band, band); x < w; x++) shade(x);
        }
    }
    if (!key.arrow) return;
    const uint32_t fill = Pixel(look_.rgb), outline = Pixel(look_.outline);
    for (size_t r = 0; r < look_.sprite.size(); r++) {
        const std::string &line = look_.sprite[r];
        for (size_t c = 0; c < line.size(); c++) {
            if (line[c] == '.') continue;
            const uint32_t px = line[c] == 'X' ? outline : fill;
            for (int y = key.ay + CellEdge(scale_y_, r); y < key.ay + CellEdge(scale_y_, r + 1) && y < h; y++)
                for (int x = key.ax + CellEdge(scale_x_, c); x < key.ax + CellEdge(scale_x_, c + 1) && x < w; x++)
                    WritePixel(buf + (size_t)y * pitch + (size_t)x * bytes, bytes, px);
        }
    }
}

void Overlay::Undraw(uint8_t *buf, size_t pitch) {
    if (!drawn_ || buf == nullptr) return;
    const unsigned bytes = fmt_.bytes;
    size_t at = under_.size();
    /* backwards, so where two spans overlap the first one saved, from the clean buffer, wins */
    for (auto s = covered_.rbegin(); s != covered_.rend(); ++s) {
        const size_t n = (size_t)(s->x1 - s->x0) * bytes;
        at -= n;
        std::memcpy(buf + (size_t)s->y * pitch + (size_t)s->x0 * bytes, &under_[at], n);
    }
    drawn_ = false;
}

const uint16_t *Overlay::Compose(uint8_t *buf, size_t pitch, const uint16_t *changed,
                                 uint32_t now_ms, bool have_pointer, float fx, float fy,
                                 bool shown) {
    if (!drawable() || buf == nullptr) return changed;
    const Key key = KeyAt(now_ms, have_pointer, fx, fy);
    Undraw(buf, pitch);                 /* if the scaler did not write, the old overlay is here */
    const bool moved = !(key == presented_);
    if (key.state != State::Off) {
        if (moved || covered_.empty()) covered_ = Covered(key);
        const unsigned bytes = fmt_.bytes;
        under_.clear();
        for (const auto &s : covered_) {
            const uint8_t *p = buf + (size_t)s.y * pitch + (size_t)s.x0 * bytes;
            under_.insert(under_.end(), p, p + (size_t)(s.x1 - s.x0) * bytes);
        }
        Draw(buf, pitch, key);
        drawn_ = true;
        frames_++;
    }
    if (shown) presented_ = key;
    return moved ? full_.data() : changed;
}

std::string Overlay::Describe() const {
    char line[160];
    /* `none` while there is no output yet, `undrawable` for one it cannot draw
       on (8-bit, TrueType text): a client tells "not yet" from "never here" */
    if (drawable())
        std::snprintf(line, sizeof(line), "PRESENCE %s frames=%llu pointer=%d output=%ux%ux%u",
                      StateName(state_), (unsigned long long)frames_, pointer_ ? 1 : 0, width_,
                      height_, fmt_.bytes * 8u);
    else
        std::snprintf(line, sizeof(line), "PRESENCE %s frames=%llu pointer=%d output=%s",
                      StateName(state_), (unsigned long long)frames_, pointer_ ? 1 : 0,
                      width_ != 0 && height_ != 0 ? "undrawable" : "none");
    return line;
}

Overlay &Instance() {
    static Overlay overlay;
    return overlay;
}

} /* namespace presence */

namespace {

/* Whether the overlay has anything to do this frame: the cheap test every frame pays. */
bool Idle() {
    const presence::Overlay &o = presence::Instance();
    return o.state() == presence::State::Off && !o.drawn() && !o.NeedsRepaint(0, false, 0, 0);
}

} /* namespace */

bool PRESENCE_Command(const std::string &args, std::vector<std::string> &lines) {
    std::string error;
    if (!presence::Instance().Configure(args, error, (uint32_t)GetTicks())) {
        lines.assign(1, error);
        return false;
    }
    lines.assign(1, presence::Instance().Describe());
    return true;
}

void PRESENCE_OutputChanged(unsigned width, unsigned height, unsigned bytes,
                            unsigned guest_width, unsigned guest_height) {
    presence::Format fmt;
    const bool ok = bytes != 0 && presence::FormatFromPixels(
            bytes, (uint32_t)GFX_GetRGB(0, 0, 0), (uint32_t)GFX_GetRGB(255, 0, 0),
            (uint32_t)GFX_GetRGB(0, 255, 0), (uint32_t)GFX_GetRGB(0, 0, 255), fmt);
    presence::Instance().Resize(width, height, ok ? &fmt : nullptr, guest_width, guest_height);
}

void PRESENCE_BeginFrame(uint8_t *buf, size_t pitch) {
    presence::Instance().Undraw(buf, pitch);
}

const uint16_t *PRESENCE_EndFrame(uint8_t *buf, size_t pitch, const uint16_t *changed,
                                  bool shown) {
    if (Idle()) return changed;
    float fx = 0, fy = 0;
    const bool pointer = Mouse_PresencePointer(fx, fy);
    return presence::Instance().Compose(buf, pitch, changed, (uint32_t)GetTicks(), pointer, fx, fy,
                                        shown);
}

bool PRESENCE_WantsRepaint() {
    if (Idle()) return false;
    float fx = 0, fy = 0;
    const bool pointer = Mouse_PresencePointer(fx, fy);
    return presence::Instance().NeedsRepaint((uint32_t)GetTicks(), pointer, fx, fy);
}

void PRESENCE_ParkedTick() {
    if (!PRESENCE_WantsRepaint()) return;
    uint8_t *pixels = nullptr;
    Bitu pitch = 0;
    if (!GFX_StartUpdate(pixels, pitch)) return;
    GFX_EndUpdate(PRESENCE_EndFrame(pixels, (size_t)pitch, nullptr));
}
