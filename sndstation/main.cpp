// Gritty Grids for sndstation — a bytebeat drum machine for Linux handhelds.
//
// A native port of ../demos/gritty-grids.html: the 25-node Grids map, the
// bilinear blend, both engines, the polyrhythm, A/R/P and Chaos, driven by a
// gamepad on a 640x480 panel instead of a mouse on a web page.
//
//   gritty-grids               run it
//   gritty-grids --dark        the demo's phosphor palette instead of paper
//   gritty-grids --res WxH     force the layout size (development)
//   gritty-grids --shots DIR   headless: write one frame per screen, then exit
//   gritty-grids --bench SEC   headless: render SEC seconds of audio, report cost
//
// The puck is the instrument. Everything else is a modifier.
#include "engine.h"
#include <sndstation/sndstation.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

using namespace sndstation;
using gfx::Font;
using namespace gritty;

namespace {

// ── the palette ──
// Every colour the app draws with lives here, including the sndstation theme
// tokens it would otherwise take straight from theme.h — the SDK's own tokens
// are a dark theme (bg0 is #0C0D0F), so a light look has to override them too.
//
// Light is the default. A handheld is used in daylight as often as not, and
// the demo's CRT green reads as a screenshot of a screenshot once it is on a
// real panel. --dark restores the demo's phosphor exactly.
struct Pal {
    SDL_Color screen;      // the map ground and the scope wells
    SDL_Color phos;        // strongest accent: live text, the puck, a flash
    SDL_Color phosD;       // mid accent: a hit, a selected chip, RUN
    SDL_Color amber;       // hot accent: the dominant node, accents, X/Y
    SDL_Color bg0;         // window
    SDL_Color bg1;         // chrome: HUD, footer, the lane grids' ground
    SDL_Color bg2;         // an inactive chip
    SDL_Color line;        // hairlines and cell frames
    SDL_Color txt0, txt1, txt2;
    SDL_Color warn;
    float     heat[3][3];  // the map's height ramp: ground, mid, peak
};

// Paper and ink. The accents keep the demo's hues and drop their value so
// they carry on a light ground; on dark, "more emphasis" means brighter, and
// here it means darker, so phos is the deepest green rather than the palest.
constexpr Pal PAL_LIGHT {
    {0xFA, 0xF8, 0xF2, 255},
    {0x0F, 0x6B, 0x38, 255},
    {0x2E, 0x96, 0x55, 255},
    {0xB2, 0x5E, 0x0E, 255},
    {0xF4, 0xF1, 0xE8, 255},
    {0xE7, 0xE3, 0xD7, 255},
    {0xD3, 0xCE, 0xBF, 255},
    {0xB2, 0xAB, 0x99, 255},
    {0x1E, 0x1C, 0x18, 255},
    {0x45, 0x42, 0x3A, 255},
    {0x7A, 0x74, 0x67, 255},
    {0xA8, 0x3A, 0x0A, 255},
    {{232, 236, 229}, {126, 188, 146}, {226, 142, 48}},
};

// The demo's CSS, unchanged: --phos, --phos-deep, --amber, and its screen.
constexpr Pal PAL_DARK {
    {0x06, 0x0D, 0x0A, 255},
    {0x7D, 0xFB, 0xA0, 255},
    {0x39, 0xC4, 0x6E, 255},
    {0xFF, 0xB3, 0x47, 255},
    {0x0C, 0x0D, 0x0F, 255},
    {0x14, 0x16, 0x1A, 255},
    {0x1D, 0x20, 0x25, 255},
    {0x2A, 0x2E, 0x33, 255},
    {0xE8, 0xE4, 0xDC, 255},
    {0x8A, 0x8F, 0x98, 255},
    {0x56, 0x5B, 0x63, 255},
    {0xFF, 0xB3, 0x00, 255},
    {{6, 13, 10}, {31, 107, 63}, {255, 179, 71}},
};

Pal P = PAL_LIGHT;

Params  g_params;
Meters  g_meters;
Engine  g_engine{g_params, g_meters};

void renderAudio(void*, float* out, int frames) { g_engine.render(out, frames); }
void rateMoved(void*, int rate) { g_engine.setRate(rate); }

// ── the controls, as one flat list the shoulder buttons walk ──
// L1/R1 move the cursor, L2/R2 change the value. Global first, then each
// lane's five, in the order the demo's lane strip shows them.
struct Ctl {
    const char*       label;
    int               lane;        // -1 = global
    std::atomic<int>* v;
    int               lo, hi, step;
};

Ctl  g_ctl[2 + 3 * 7];
int  g_nCtl = 0;
int  g_cursor = 0;

void buildControls() {
    g_nCtl = 0;
    g_ctl[g_nCtl++] = {"BPM",   -1, &g_params.bpm,   40, 240, 1};
    g_ctl[g_nCtl++] = {"CHAOS", -1, &g_params.chaos,  0, 200, 2};
    for (int i = 0; i < 3; ++i) {
        g_ctl[g_nCtl++] = {"F", i, &g_params.freeRun[i], 0,   1, 1};
        g_ctl[g_nCtl++] = {"K", i, &g_params.key[i],   -24,  24, 1};
        g_ctl[g_nCtl++] = {"D", i, &g_params.dens[i], 0, 255, 2};
        g_ctl[g_nCtl++] = {"L", i, &g_params.len[i],  2,  16, 1};
        g_ctl[g_nCtl++] = {"A", i, &g_params.atk[i],  0, 100, 1};
        g_ctl[g_nCtl++] = {"R", i, &g_params.rel[i],  0, 100, 1};
        g_ctl[g_nCtl++] = {"P", i, &g_params.prob[i], 0, 100, 1};
    }
}
// A control the current mode has no use for is skipped by the cursor, the way
// the demo hides L outside Formula and P outside Bit voice.
bool ctlLive(const Ctl& c) {
    // A free lane has no sequencer and no envelope, so the five controls that
    // drive them are not dimmed, they are gone: only F and its key remain.
    if (c.lane >= 0 && g_params.isFree(c.lane))
        return c.label[0] == 'F' || c.label[0] == 'K';
    if (c.label[0] == 'K') return false;
    if (c.label[0] == 'L' && c.label[1] == 0)
        return Rhythm(g_params.rhythm.load()) == Rhythm::Formula;
    if (c.label[0] == 'P' && c.label[1] == 0)
        return Mode(g_params.mode.load()) == Mode::Voice;
    return true;
}
void moveCursor(int d) {
    for (int n = 0; n < g_nCtl; ++n) {
        g_cursor = (g_cursor + d + g_nCtl) % g_nCtl;
        if (ctlLive(g_ctl[g_cursor])) return;
    }
}
void nudge(int dir, int mult) {
    const Ctl& c = g_ctl[g_cursor];
    int v = c.v->load() + dir * c.step * mult;
    if (v < c.lo) v = c.lo;
    if (v > c.hi) v = c.hi;
    c.v->store(v);
}

// How a control reads out, in the units the demo shows.
std::string ctlValue(const Ctl& c) {
    char buf[32];
    const int v = c.v->load();
    const int i = c.lane;
    if (c.lane < 0) {
        if (c.label[0] == 'B') { std::snprintf(buf, sizeof buf, "%d", v); return buf; }
        std::snprintf(buf, sizeof buf, "%d%%", int(std::lround(v / 200.0 * 100)));
        return buf;
    }
    switch (c.label[0]) {
        case 'F': return v ? "FREE" : "SEQ";
        case 'K': {
            const char* mark = keyAligned(v) ? "" : "*";   // * = off the beat grid
            std::snprintf(buf, sizeof buf, "%+d%s", v, mark);
            return buf;
        }
        case 'D':
            if (Mode(g_params.mode.load()) == Mode::Voice)
                std::snprintf(buf, sizeof buf, "\xC3\x97%.2f", g_params.voiceRate(i));
            else
                std::snprintf(buf, sizeof buf, "%d%%", int(std::lround(v / 255.0 * 100)));
            return buf;
        case 'L': std::snprintf(buf, sizeof buf, "%d", v); return buf;
        case 'A': std::snprintf(buf, sizeof buf, "%dms", int(std::lround(g_params.envAtk(i) * 1000))); return buf;
        case 'R': std::snprintf(buf, sizeof buf, "%dms", int(std::lround(g_params.envRel(i) * 1000))); return buf;
        default:
            if (g_params.ratchetN(i) > 1) std::snprintf(buf, sizeof buf, "\xC3\x97%d", g_params.ratchetN(i));
            else std::snprintf(buf, sizeof buf, "%d%%", int(std::lround(g_params.probChance(i) * 100)));
            return buf;
    }
}

// ── destinations on the D-pad ──
// A destination is a whole patch, not a place: the puck's position and every
// parameter — BPM, Chaos, both engine switches, and all five controls on each
// of the three lanes. Binding the position alone turned out to be the lesser
// half of the idea, since the map only sets the pattern and the lane controls
// are what make it a sound.
//
// Hold L3 and press a direction to bind the current state to it; R3 and a
// direction unbinds. A bound direction then travels there, and travel is a
// glide rather than a jump — the continuous values ramp together, so a recall
// is a morph between two patches and the sweep is itself worth hearing. The
// two switches (engine, rhythm source) cannot be interpolated, so they snap at
// the start and you hear the destination's engine while the rest arrives.
//
// The stick interrupts a glide at any point, which leaves every value halfway
// between the two patches. That is a feature: half of a morph is a patch you
// did not have.
struct Snap {
    bool set = false;
    int  x = 128, y = 128, bpm = 120, chaos = 30, mode = 0, rhythm = 0;
    int  dens[3]{}, len[3]{}, atk[3]{}, rel[3]{}, prob[3]{};
};
Snap  g_dest[4];                       // Up, Down, Left, Right
Snap  g_from;                          // where a glide started
int   g_travel = -1;                   // slot being travelled to, -1 = none
float g_travelT = 0, g_travelDur = 0;
int   g_bindHint = 0;                  // 0 none, 1 binding, 2 clearing
constexpr float TRAVEL_RATE = 620.0f;  // map units a second, before clamping

Snap captureSnap() {
    Snap s;
    s.set = true;
    s.x = g_params.x.load();     s.y      = g_params.y.load();
    s.bpm = g_params.bpm.load(); s.chaos  = g_params.chaos.load();
    s.mode = g_params.mode.load(); s.rhythm = g_params.rhythm.load();
    for (int i = 0; i < 3; ++i) {
        s.dens[i] = g_params.dens[i].load(); s.len[i]  = g_params.len[i].load();
        s.atk[i]  = g_params.atk[i].load();  s.rel[i]  = g_params.rel[i].load();
        s.prob[i] = g_params.prob[i].load();
    }
    return s;
}
int mix(int a, int b, float t) { return int(std::lround(a + (b - a) * double(t))); }

// Everything that can be ramped. x and y are handled by the caller, which owns
// the puck's float position and has to keep the stick in step with it.
void applySnapMix(const Snap& a, const Snap& b, float t) {
    g_params.bpm.store(mix(a.bpm, b.bpm, t));
    g_params.chaos.store(mix(a.chaos, b.chaos, t));
    for (int i = 0; i < 3; ++i) {
        g_params.dens[i].store(mix(a.dens[i], b.dens[i], t));
        g_params.len[i].store(mix(a.len[i],  b.len[i],  t));
        g_params.atk[i].store(mix(a.atk[i],  b.atk[i],  t));
        g_params.rel[i].store(mix(a.rel[i],  b.rel[i],  t));
        g_params.prob[i].store(mix(a.prob[i], b.prob[i], t));
    }
}

// On a handheld the pad's own labels are right. On a desktop with no pad
// attached they are a lie — the footer said "L1/R1 pick" at someone pressing
// keys, which is exactly the hint they needed and could not use.
bool g_padPresent = false;
const char* btn(pad::Button b) {
    if (g_padPresent) return pad::label(b);
    switch (b) {
        case pad::East:   return "Z";
        case pad::South:  return "X";
        case pad::North:  return "C";
        case pad::West:   return "V";
        case pad::L1:     return "1";
        case pad::R1:     return "4";
        case pad::L2:     return "2";
        case pad::R2:     return "3";
        case pad::L3:     return "E";
        case pad::R3:     return "I";
        case pad::Select: return "BKSP";
        case pad::Start:  return "RET";
        default:          return pad::label(b);
    }
}
const char* slotName(int slot) {
    static const char* n[4] = {"UP", "DOWN", "LEFT", "RIGHT"};
    return (slot >= 0 && slot < 4) ? n[slot] : "";
}

int destSlot(pad::Button b) {
    switch (b) {
        case pad::Up:    return 0;
        case pad::Down:  return 1;
        case pad::Left:  return 2;
        case pad::Right: return 3;
        default:         return -1;
    }
}
// Which way the marker's pip sits off its centre. Open Sans has no arrow
// glyphs at this size, so the direction is drawn rather than typed.
void destPip(int slot, float& dx, float& dy) {
    const float v[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
    dx = v[slot][0]; dy = v[slot][1];
}
void clearDests() { for (Snap& d : g_dest) d = Snap{}; g_travel = -1; }

// ── the topographic heat field ──
// Static: the node set never changes, so the field is rasterized once into a
// texture and blitted. Only the puck on top of it moves.
SDL_Texture* g_field = nullptr;
constexpr int FW = 140;

float scalarAt(float gx, float gy) {
    const Curves& cv = curves();
    int xi = int(gx); if (xi > 3) xi = 3;
    int yi = int(gy); if (yi > 3) yi = 3;
    const float fx = gx - float(xi), fy = gy - float(yi);
    const float a = cv.scalar[yi * 5 + xi],     b = cv.scalar[yi * 5 + xi + 1];
    const float c = cv.scalar[(yi + 1) * 5 + xi], d = cv.scalar[(yi + 1) * 5 + xi + 1];
    const float top = a + (b - a) * fx, bot = c + (d - c) * fx;
    return top + (bot - top) * fy;
}
void heatRamp(float s, float& r, float& g, float& b) {
    const float* c0 = P.heat[0]; const float* c1 = P.heat[1]; const float* c2 = P.heat[2];
    const float* a; const float* z; float f;
    if (s < 0.6f) { a = c0; z = c1; f = s / 0.6f; }
    else          { a = c1; z = c2; f = (s - 0.6f) / 0.4f; }
    r = a[0] + (z[0] - a[0]) * f;
    g = a[1] + (z[1] - a[1]) * f;
    b = a[2] + (z[2] - a[2]) * f;
}
void buildField(SDL_Renderer* ren) {
    static uint32_t px[FW * FW];
    for (int py = 0; py < FW; ++py)
        for (int pxx = 0; pxx < FW; ++pxx) {
            const float gx = float(pxx) / (FW - 1) * 4, gy = float(py) / (FW - 1) * 4;
            const float s = scalarAt(gx, gy);
            float r, g, b;
            heatRamp(s, r, g, b);
            const float band = (int(std::floor(s * 9)) % 2 == 0) ? 1.0f : 0.86f;   // contours
            px[py * FW + pxx] = 0xFF000000u | (uint32_t(r * band) << 16) |
                                (uint32_t(g * band) << 8) | uint32_t(b * band);
        }
    g_field = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, FW, FW);
    if (g_field) SDL_UpdateTexture(g_field, nullptr, px, FW * 4);
}

// ── the frame ──
struct Flash { uint32_t seen = 0; int idx = -1; float t = 0; bool acc = false; };
Flash g_flash[3];

void drawMap(SDL_Renderer* r, int bx, int by, int size) {
    using namespace theme;
    const Corners c = cornersAt(g_params.x.load(), g_params.y.load());
    const SDL_Rect dst{bx, by, size, size};
    gfx::fillRect(r, dst, P.screen);
    if (g_field) gfx::blit(r, g_field, nullptr, &dst);
    gfx::frameRect(r, dst, P.line);

    // the 25 nodes
    for (int gy = 0; gy < 5; ++gy)
        for (int gx = 0; gx < 5; ++gx)
            gfx::disc(r, float(bx) + gx / 4.0f * size, float(by) + gy / 4.0f * size,
                      suf(1.8f), withAlpha(P.phos, 150));

    // the four that are actually sounding, sized by weight
    for (int k = 0; k < 4; ++k) {
        if (c.w[k] < 0.02f) continue;
        const int n = c.idx[k];
        const float gx = float(n % 5), gy = float(n / 5);
        gfx::disc(r, float(bx) + gx / 4.0f * size, float(by) + gy / 4.0f * size,
                  suf(2.0f + 4.0f * c.w[k]), withAlpha(P.amber, Uint8(80 + 175 * c.w[k])));
    }

    // bound destinations, drawn under the puck
    for (int k = 0; k < 4; ++k) {
        if (!g_dest[k].set) continue;
        const float dx = float(bx) + g_dest[k].x / 255.0f * size;
        const float dy = float(by) + g_dest[k].y / 255.0f * size;
        const bool  going = k == g_travel;
        const SDL_Color ink = going ? P.amber : P.phos;
        float ox, oy; destPip(k, ox, oy);
        gfx::disc(r, dx, dy, suf(7.0f), ink);
        gfx::disc(r, dx, dy, suf(5.2f), P.screen);
        gfx::disc(r, dx + ox * suf(7.0f), dy + oy * suf(7.0f), suf(2.6f), ink);
    }

    // while a bind is armed, say so over the map rather than in the footer
    if (g_bindHint) {
        const char* msg = g_bindHint == 1 ? "press a direction to bind this spot"
                                          : "press a direction to clear it";
        const int th = su(16);
        gfx::fillRect(r, {bx, by + size - th, size, th}, P.bg1);
        gfx::textMid(r, Font::Small, msg, bx + size / 2, by + size - th / 2,
                     g_bindHint == 1 ? P.phos : P.amber, 0);
    }

    // crosshair + puck
    const float px = float(bx) + g_params.x.load() / 255.0f * size;
    const float py = float(by) + g_params.y.load() / 255.0f * size;
    gfx::fillRect(r, {int(px), by, 1, size}, withAlpha(P.phos, 70));
    gfx::fillRect(r, {bx, int(py), size, 1}, withAlpha(P.phos, 70));
    gfx::glow(r, px, py, suf(13), withAlpha(P.phos, 110));
    gfx::disc(r, px, py, suf(7), P.phos);
    gfx::disc(r, px, py, suf(2.6f), P.screen);
}

void drawBlend(SDL_Renderer* r, int bx, int by, int w) {
    using namespace theme;
    const int X = g_params.x.load(), Y = g_params.y.load();
    const Corners c = cornersAt(X, Y);
    const int dom = domNode(X, Y);
    const Node* N = nodes();

    char hdr[64];
    std::snprintf(hdr, sizeof hdr, "node %d \xC2\xB7 %s", dom, N[dom].author);
    gfx::textMid(r, Font::Small, hdr, bx, by + su(6), P.txt2);
    gfx::textMid(r, Font::Label, N[dom].expr, bx, by + su(24), P.phos);

    char coord[32];
    std::snprintf(coord, sizeof coord, "X %d \xC2\xB7 Y %d", X, Y);
    gfx::textMid(r, Font::Small, coord, bx + w, by + su(6), P.amber, 1);

    // the blend, heaviest corner first
    int ord[4] = {0, 1, 2, 3};
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
            if (c.w[ord[j]] > c.w[ord[i]]) { const int t = ord[i]; ord[i] = ord[j]; ord[j] = t; }

    int y = by + su(44);
    for (int i = 0; i < 4; ++i) {
        const int   k  = ord[i];
        const int   ni = c.idx[k];
        const int   pct = int(std::lround(c.w[k] * 100));
        const bool  isDom = ni == dom;
        char tag[16];
        std::snprintf(tag, sizeof tag, "n%d", ni);
        gfx::textMid(r, Font::Small, tag, bx, y + su(7), isDom ? P.amber : P.txt2);
        gfx::textMid(r, Font::Small, N[ni].expr, bx + su(26), y + su(7),
                     isDom ? P.txt0 : P.txt1);
        char p[8];
        std::snprintf(p, sizeof p, "%d%%", pct);
        gfx::textMid(r, Font::Small, p, bx + w, y + su(7), isDom ? P.amber : P.txt1, 1);
        const int barW = w - su(26);
        gfx::fillRect(r, {bx + su(26), y + su(16), barW, su(3)}, P.bg2);
        gfx::fillRect(r, {bx + su(26), y + su(16), barW * pct / 100, su(3)},
                      isDom ? P.amber : P.phosD);
        y += su(26);
    }
}

void drawLanes(SDL_Renderer* r, int bx, int by, int w, float dt) {
    using namespace theme;
    const Mode   mode   = Mode(g_params.mode.load());
    const Rhythm rhythm = Rhythm(g_params.rhythm.load());
    const Corners c = cornersAt(g_params.x.load(), g_params.y.load());
    const bool running = g_params.playing.load();

    const int nameW = su(74);
    const int cellsX = bx + nameW;
    const int cellsW = w - nameW - su(96);
    const int rowH   = su(34);

    for (int vi = 0; vi < 3; ++vi) {
        const int y = by + vi * rowH;
        gfx::textMid(r, Font::Label, laneName(vi), bx, y + su(9),
                     g_ctl[g_cursor].lane == vi ? P.phos : P.txt1);
        const bool freeLane = g_params.isFree(vi);
        gfx::textMid(r, Font::Small, freeLane ? "no sequencer" : laneRole(vi, mode),
                     bx, y + su(21), freeLane ? P.amber : P.txt2);

        const int Ln   = g_params.laneLen(vi);
        const float dens = float(g_params.dens[vi].load());
        // What the grid shows is what the running engine will actually fire.
        // Bit voice ignores density (that control is PITCH there) and gates on
        // the role or a fixed half-range threshold — the web demo draws the
        // density pattern in both engines, which is the one place its grid
        // and its sound disagree.
        const bool  voice = mode == Mode::Voice;
        const float thrF  = rhythm == Rhythm::Formula
                                ? trackThreshold(c, vi, Ln, voice ? 0.5f : dens / 255.0f)
                                : 255.0f - (voice ? 255.0f : dens);
        const int cw = cellsW / Ln;

        // the flash: the audio thread bumped a hit counter, so light the cell
        // its playhead was on. This is the only path from DSP to screen.
        const uint32_t h = g_meters.hits[vi].load();
        if (h != g_flash[vi].seen) {
            g_flash[vi].seen = h;
            g_flash[vi].idx  = g_meters.step[vi].load();
            g_flash[vi].t    = 0.19f;
            g_flash[vi].acc  = false;
        }
        if (g_flash[vi].t > 0) g_flash[vi].t -= dt;

        const int ph = running ? g_meters.step[vi].load() : -1;
        // A free lane has no steps to show. Saying so is more honest than
        // leaving a grid up that nothing is reading.
        if (freeLane) {
            const SDL_Rect band{cellsX, y + su(4), cellsW, su(18)};
            gfx::fillRect(r, band, P.bg2);
            gfx::frameRect(r, band, P.line);
            const int    kv  = g_params.key[vi].load();
            const double br  = freeBaseRate(g_params.bpm.load());
            const double per = br / (double(g_params.bpm.load()) / 60.0);
            char caption[72];
            std::snprintf(caption, sizeof caption,
                          "the formula is playing itself \xC2\xB7 %d per beat \xC2\xB7 %+d st%s",
                          int(std::lround(per)), kv,
                          keyAligned(kv) ? "" : " \xC2\xB7 off the grid");
            gfx::textMid(r, Font::Small, caption, cellsX + cellsW / 2, y + su(13),
                         P.txt2, 0);
        }
        for (int s = 0; !freeLane && s < Ln; ++s) {
            const SDL_Rect cell{cellsX + s * cw, y + su(4), cw - su(2), su(18)};
            float L;
            bool  on;
            if (rhythm == Rhythm::Formula) {
                L  = grooveLevel(c, vi, float(s) / float(Ln));
                on = L > thrF;
            } else {
                L  = levelAt(c, vi, s);
                on = candidate(vi, s) && L > thrF;
            }
            const bool cand = rhythm == Rhythm::Roles && candidate(vi, s);
            const bool beat = rhythm == Rhythm::Roles && s % 4 == 0;
            SDL_Color fill = P.bg1;
            if (cand) fill = P.bg2;
            if (on)   fill = (L > 212.0f) ? P.amber : P.phosD;
            if (g_flash[vi].t > 0 && g_flash[vi].idx == s) fill = P.phos;
            gfx::fillRect(r, cell, fill);
            if (beat) gfx::fillRect(r, {cell.x, cell.y, su(2), cell.h}, withAlpha(P.phos, 60));
            gfx::frameRect(r, cell, s == ph ? P.phos : P.line);
        }

        // The lane's own trace: 1-bit in Bit voice, the formula's actual
        // waveform when the lane is free. Mapped linearly either way, so a
        // square still reads as two rails and a free lane reads as itself.
        if (mode == Mode::Voice || freeLane) {
            const int sx = cellsX + cellsW + su(8), sw = su(80), sh = su(18);
            const int mid = y + su(4) + sh / 2;
            gfx::fillRect(r, {sx, y + su(4), sw, sh}, P.screen);
            const int start = g_meters.scopePtr[vi].load();
            for (int j = 0; j < sw; ++j) {
                const int   si = (start + j * SCOPE_N / sw) & (SCOPE_N - 1);
                float       v  = g_meters.scope[vi][si];
                if (v < -1) v = -1;
                if (v >  1) v =  1;
                int yy = mid - int(v * (sh * 0.42f));
                if (yy < y + su(4)) yy = y + su(4);
                if (yy > y + su(4) + sh - su(2)) yy = y + su(4) + sh - su(2);
                gfx::fillRect(r, {sx + j, yy, 1, su(2)}, freeLane ? P.amber : P.phos);
            }
            gfx::frameRect(r, {sx, y + su(4), sw, sh}, P.line);
        }
    }
}

void drawStrip(SDL_Renderer* r, int bx, int by, int w) {
    using namespace theme;
    const int focusLane = g_ctl[g_cursor].lane;
    gfx::fillRect(r, {bx, by, w, su(26)}, P.bg1);

    int x = bx + su(6);
    for (int i = 0; i < g_nCtl; ++i) {
        const Ctl& c = g_ctl[i];
        if (!ctlLive(c)) continue;
        if (c.lane >= 0 && c.lane != focusLane) continue;   // only the focused lane's five
        const bool sel = i == g_cursor;
        char label[48];
        if (c.lane < 0) std::snprintf(label, sizeof label, "%s %s", c.label, ctlValue(c).c_str());
        else            std::snprintf(label, sizeof label, "%s %s", c.label, ctlValue(c).c_str());
        const int tw = gfx::textWidth(Font::Small, label) + su(14);
        if (x + tw > bx + w) break;
        gfx::fillRect(r, {x, by + su(3), tw, su(20)}, sel ? P.phosD : P.bg2);
        gfx::textMid(r, Font::Small, label, x + tw / 2, by + su(13), sel ? P.screen : P.txt1, 0);
        x += tw + su(5);
    }
    if (focusLane >= 0) {
        gfx::textMid(r, Font::Small, laneName(focusLane), bx + w - su(6), by + su(13), P.amber, 1);
    }
}

void draw(SDL_Renderer* r, const AudioOut& out, float dt) {
    using namespace theme;
    gfx::clear(r, P.bg0);

    const Mode   mode    = Mode(g_params.mode.load());
    const Rhythm rhythm  = Rhythm(g_params.rhythm.load());
    const bool   running = g_params.playing.load();

    // HUD
    gfx::fillRect(r, {0, 0, SCREEN_W, HUD_H}, P.bg1);
    gfx::textMid(r, Font::Title, "GRITTY GRIDS", su(16), HUD_H / 2, P.txt0);
    int hx = su(16) + gfx::textWidth(Font::Title, "GRITTY GRIDS") + su(14);
    const char* pills[2] = {mode == Mode::Voice ? "BIT VOICE" : "BITSEQ",
                            rhythm == Rhythm::Formula ? "FORMULA" : "ROLES"};
    for (const char* p : pills) {
        const int tw = gfx::textWidth(Font::Small, p) + su(14);
        gfx::fillRect(r, {hx, HUD_H / 2 - su(9), tw, su(18)}, P.bg2);
        gfx::textMid(r, Font::Small, p, hx + tw / 2, HUD_H / 2, P.phos, 0);
        hx += tw + su(6);
    }
    if (g_travel >= 0) {
        char t[32];
        const float u = g_travelDur > 0 ? g_travelT / g_travelDur : 1.0f;
        std::snprintf(t, sizeof t, "> %s %d%%", slotName(g_travel),
                      int(std::lround((u > 1 ? 1 : u) * 100)));
        const int tw = gfx::textWidth(Font::Small, t) + su(14);
        gfx::fillRect(r, {hx, HUD_H / 2 - su(9), tw, su(18)}, P.amber);
        gfx::textMid(r, Font::Small, t, hx + tw / 2, HUD_H / 2, P.screen, 0);
        hx += tw + su(6);
    }

    const char* tr = running ? "RUN" : "STOP";
    const int trw = gfx::textWidth(Font::Small, tr) + su(16);
    gfx::fillRect(r, {SCREEN_W - su(16) - trw, HUD_H / 2 - su(9), trw, su(18)},
                  running ? P.phosD : P.bg2);
    gfx::textMid(r, Font::Small, tr, SCREEN_W - su(16) - trw / 2, HUD_H / 2,
                 running ? P.screen : P.txt2, 0);
    gfx::hline(r, 0, HUD_H, SCREEN_W, P.line);

    // the map, and what it is blending
    const int mapSize = su(188);
    drawMap(r, su(16), HUD_H + su(12), mapSize);
    drawBlend(r, su(16) + mapSize + su(16), HUD_H + su(12),
              SCREEN_W - su(32) - mapSize - su(16));

    // the three lanes, and the focused lane's controls
    const int lanesY = HUD_H + su(12) + mapSize + su(14);
    drawLanes(r, su(16), lanesY, SCREEN_W - su(32), dt);
    drawStrip(r, su(16), SCREEN_H - FOOTER_H - su(34), SCREEN_W - su(32));

    // footer
    const int fy = SCREEN_H - FOOTER_H;
    gfx::fillRect(r, {0, fy, SCREEN_W, FOOTER_H}, P.bg1);
    const std::string row1 = std::string(btn(pad::East)) + " run   " +
                             btn(pad::South) + " engine   " +
                             btn(pad::North) + " rhythm   " +
                             btn(pad::West) + " lane   " +
                             btn(pad::L1) + "/" + btn(pad::R1) + " pick   " +
                             btn(pad::L2) + "/" + btn(pad::R2) + " set";
    const std::string row2 = std::string(btn(pad::L3)) + "+dir bind patch   " +
                             btn(pad::R3) + "+dir clear   dir recall   " +
                             btn(pad::Select) + " reset   " +
                             btn(pad::Start) + " quit";
    gfx::textMid(r, Font::Small, row1, su(16), fy + FOOTER_H / 2 - su(8), P.txt1);
    gfx::textMid(r, Font::Small, row2, su(16), fy + FOOTER_H / 2 + su(8), P.txt2);
    if (out.deviceOpen()) {
        char a[48];
        std::snprintf(a, sizeof a, "%d Hz \xC2\xB7 %d", out.sampleRate(), out.deviceFrames());
        gfx::textMid(r, Font::Small, a, SCREEN_W - su(16), fy + FOOTER_H / 2, P.txt2, 1);
    } else {
        gfx::textMid(r, Font::Small, "no audio device", SCREEN_W - su(16),
                     fy + FOOTER_H / 2, P.warn, 1);
    }
}

// ── headless: render audio with no device, and say what it cost ──
int bench(double seconds) {
    const int sr = 48000, block = 256;
    g_engine.setRate(sr);
    static float buf[block * 2];
    const long blocks = long(seconds * sr / block);
    int worst = 0;
    // the third pass is all three lanes free: a different code path, and the
    // one with no envelopes, no grains and no filters in it
    for (int mode = 0; mode < 3; ++mode) {
        g_params.mode.store(mode == 2 ? int(Mode::Bitseq) : mode);
        for (int i = 0; i < 3; ++i) g_params.freeRun[i].store(mode == 2 ? 1 : 0);
        g_params.playing.store(true);
        g_engine.rewind();
        const auto t0 = std::chrono::steady_clock::now();
        double peak = 0, sumSq = 0;
        long   clipped = 0, total = 0;
        for (long b = 0; b < blocks; ++b) {
            // "at full scale" means |x| >= 0.999. For bitseq and bit-voice that
            // is the clipper engaging. For free lanes there is no clipper: a
            // byte of 255 is exactly +1.0, so those samples are the formula's
            // own range arriving intact, which is the point of the mode.
            //
            // sweep the puck across the whole map while it runs: the worst
            // case is a moving position, which rebuilds a wavetable per hit.
            g_params.x.store(int(255.0 * double(b) / double(blocks)));
            g_params.y.store(int(255.0 * (0.5 + 0.5 * std::sin(double(b) * 0.01))));
            std::memset(buf, 0, sizeof buf);
            g_engine.render(buf, block);
            for (int k = 0; k < block; ++k) {          // one channel: they are identical
                const double v = buf[2 * k], a = std::fabs(v);
                if (a > peak) peak = a;
                if (a >= 0.999) ++clipped;
                sumSq += v * v;
                ++total;
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        const double realtime = seconds * 1000.0;
        std::printf("%-10s %6.1f ms for %.1f s audio = %5.2f%% of one core   "
                    "peak %.2f  rms %.3f  at full scale %.3f%%\n",
                    mode == 2 ? "free x3" : (mode ? "bit-voice" : "bitseq"),
                    ms, seconds, ms / realtime * 100.0,
                    peak, std::sqrt(sumSq / double(total)),
                    double(clipped) / double(total) * 100.0);
        if (peak < 0.01) { std::printf("  silent: that is a bug\n"); worst = 1; }
    }
    g_params.playing.store(false);
    for (int i = 0; i < 3; ++i) g_params.freeRun[i].store(0);
    return worst;
}

} // namespace

int main(int argc, char** argv) {
    WindowOptions wo;
    wo.title = "gritty-grids";
    std::string shots;
    double benchSec = 0;
    for (int i = 1; i < argc; ++i) {
        if      (!std::strcmp(argv[i], "--shots") && i + 1 < argc) shots = argv[++i];
        else if (!std::strcmp(argv[i], "--bench") && i + 1 < argc) benchSec = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--dark")) P = PAL_DARK;
        else if (!std::strcmp(argv[i], "--res")   && i + 1 < argc)
            std::sscanf(argv[++i], "%dx%d", &wo.width, &wo.height);
    }
    buildControls();
    if (benchSec > 0) return bench(benchSec);

    wo.headless = !shots.empty();
    Window win;
    if (!win.open(wo)) return 1;
    buildField(win.renderer);
    // with no pad attached the player is on the keyboard, so name the keys
    g_padPresent = SDL_NumJoysticks() > 0;

    AudioOut out;
    const AudioOut::Settings set = AudioOut::loadSettings();
    rt::applySettings(set.realtimePriority, set.cpu);
    out.open(set, &renderAudio, &rateMoved, nullptr, !wo.headless);
    g_engine.setRate(out.sampleRate());
    g_engine.rewind();
    out.start();

    pad::State st;

    if (wo.headless) {
        // One frame per screen state worth looking at, so a change to the
        // layout can be reviewed with no handheld and no display.
        // dests: bind a few waypoints so the shot shows the markers, one of
        // them mid-travel (amber) and the rest idle.
        struct Shot { const char* name; Mode m; Rhythm r; bool run; int cursor;
                      bool dests; int freeMask; };
        const Shot shots_[] = {
            {"bitseq-roles",    Mode::Bitseq, Rhythm::Roles,   false, 0,  false, 0},
            {"bitseq-formula",  Mode::Bitseq, Rhythm::Formula, true,  3,  true,  0},
            {"voice-roles",     Mode::Voice,  Rhythm::Roles,   true,  6,  false, 0},
            {"voice-formula",   Mode::Voice,  Rhythm::Formula, true,  11, false, 0},
            {"free-lanes",      Mode::Bitseq, Rhythm::Formula, true,  2,  false, 0b101},
        };
        int bad = 0;
        for (const Shot& s : shots_) {
            g_params.mode.store(int(s.m));
            g_params.rhythm.store(int(s.r));
            g_params.playing.store(s.run);
            g_params.x.store(s.run ? 196 : 128);
            g_params.y.store(s.run ? 72  : 128);
            for (int i = 0; i < 3; ++i) {
                g_params.freeRun[i].store((s.freeMask >> i) & 1);
                g_params.key[i].store(KEY_DEF[i]);
            }
            g_cursor = s.cursor;
            clearDests();
            if (s.dests) {
                Snap a = captureSnap(); a.x =  58; a.y =  36; g_dest[0] = a;
                Snap b = captureSnap(); b.x =  34; b.y = 214; g_dest[2] = b;
                Snap c = captureSnap(); c.x = 232; c.y =  96;
                c.bpm = 180; c.chaos = 150;             // a patch, not a place
                for (int i = 0; i < 3; ++i) { c.dens[i] = 90; c.rel[i] = 70; }
                g_dest[3] = c;
                g_from = captureSnap();
                g_travel = 3; g_travelDur = 1.0f; g_travelT = 0.5f;
                applySnapMix(g_from, c, 0.5f);          // caught mid-morph
                g_params.x.store(mix(g_from.x, c.x, 0.5f));
                g_params.y.store(mix(g_from.y, c.y, 0.5f));
            }
            if (!ctlLive(g_ctl[g_cursor])) moveCursor(1);
            if (s.run) {                       // let the engine put something on the grid
                static float b[512];
                g_engine.rewind();
                for (int k = 0; k < 40; ++k) { std::memset(b, 0, sizeof b); g_engine.render(b, 256); }
            }
            draw(win.renderer, out, 1.0f / 60.0f);
            const std::string path = shots + "/gritty-grids-" + s.name + ".bmp";
            if (win.saveBMP(path)) std::printf("shot: %s\n", path.c_str());
            else { std::printf("could not write %s\n", path.c_str()); bad = 1; }
            win.beginFrame();
        }
        g_params.playing.store(false);
        out.close();
        return bad;
    }

    // ── the loop ──
    // The puck is analog on the stick and stepped on the D-pad; the shoulders
    // walk and change one control at a time, with a hold-to-repeat that gets
    // coarser the longer it is held.
    float  puckX = float(g_params.x.load()), puckY = float(g_params.y.load());
    double held = 0;
    int    heldDir = 0;
    Uint32 last = SDL_GetTicks();

    for (bool running = true; running;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            pad::Event ev[8];
            const int n = pad::translate(e, st, ev);
            for (int i = 0; i < n; ++i) {
                if (!ev[i].pressed) continue;
                switch (ev[i].b) {
                    case pad::Start: running = false; break;
                    case pad::East:                               // run / stop
                        if (g_params.playing.load()) {
                            g_params.playing.store(false);
                        } else {
                            g_engine.rewind();
                            g_params.playing.store(true);
                        }
                        break;
                    case pad::South:                              // engine
                        g_params.mode.store(g_params.mode.load() == int(Mode::Bitseq)
                                                ? int(Mode::Voice) : int(Mode::Bitseq));
                        g_engine.rewind();
                        if (!ctlLive(g_ctl[g_cursor])) moveCursor(1);
                        break;
                    case pad::North:                              // rhythm source
                        g_params.rhythm.store(g_params.rhythm.load() == int(Rhythm::Roles)
                                                  ? int(Rhythm::Formula) : int(Rhythm::Roles));
                        if (!ctlLive(g_ctl[g_cursor])) moveCursor(1);
                        break;
                    case pad::West: {                             // next lane's controls
                        const int from = g_ctl[g_cursor].lane;
                        for (int k = 0; k < g_nCtl; ++k) {
                            moveCursor(1);
                            if (g_ctl[g_cursor].lane != from) break;
                        }
                        break;
                    }
                    case pad::L1: moveCursor(-1); break;
                    case pad::R1: moveCursor(+1); break;
                    case pad::L2: nudge(-1, 1); heldDir = -1; held = 0; break;
                    case pad::R2: nudge(+1, 1); heldDir = +1; held = 0; break;
                    case pad::Up: case pad::Down:
                    case pad::Left: case pad::Right: {        // destinations
                        const int slot = destSlot(ev[i].b);
                        if (slot < 0) break;
                        if (st.down(pad::L3)) {
                            g_dest[slot] = captureSnap();
                            g_travel = -1;
                        } else if (st.down(pad::R3)) {
                            g_dest[slot] = Snap{};
                            if (g_travel == slot) g_travel = -1;
                        } else if (g_dest[slot].set) {
                            const Snap& d = g_dest[slot];
                            g_from = captureSnap();
                            g_travel = slot;
                            g_travelT = 0;
                            // a long hop takes longer, within reason
                            const float dx = float(d.x - g_from.x), dy = float(d.y - g_from.y);
                            const float dist = std::sqrt(dx * dx + dy * dy);
                            g_travelDur = dist / TRAVEL_RATE;
                            if (g_travelDur < 0.18f) g_travelDur = 0.18f;
                            if (g_travelDur > 0.90f) g_travelDur = 0.90f;
                            // the two switches cannot be interpolated: snap them
                            // now, so the destination's engine carries the morph
                            if (d.mode != g_from.mode) {
                                g_params.mode.store(d.mode);
                                g_engine.rewind();
                            }
                            g_params.rhythm.store(d.rhythm);
                            if (!ctlLive(g_ctl[g_cursor])) moveCursor(1);
                        }
                        break;
                    }
                    case pad::Select:                             // back to the defaults
                        g_params.reset();
                        clearDests();
                        puckX = float(g_params.x.load());
                        puckY = float(g_params.y.load());
                        break;
                    default: break;
                }
            }
        }

        const Uint32 now = SDL_GetTicks();
        const float dt = float(now - last) / 1000.0f;
        last = now;
        pad::update(st, dt);

        // the puck: stick is a velocity, a D-pad direction is a step until
        // something is bound to it, and then it is a glide to that spot
        const float speed = 150.0f;    // units per second at full deflection
        g_bindHint = st.down(pad::L3) ? 1 : (st.down(pad::R3) ? 2 : 0);
        if (std::fabs(st.lx) > 0.12f || std::fabs(st.ly) > 0.12f) g_travel = -1;
        puckX += st.lx * speed * dt;
        puckY -= st.ly * speed * dt;   // stick y is positive up; the map is not
        if (g_travel >= 0) {
            const Snap& d = g_dest[g_travel];
            g_travelT += dt;
            float u = g_travelDur > 0 ? g_travelT / g_travelDur : 1.0f;
            if (u > 1.0f) u = 1.0f;
            const float e = u * u * (3.0f - 2.0f * u);     // ease in and out
            puckX = float(g_from.x) + (float(d.x) - float(g_from.x)) * e;
            puckY = float(g_from.y) + (float(d.y) - float(g_from.y)) * e;
            applySnapMix(g_from, d, e);
            if (u >= 1.0f) {
                g_travel = -1;
                if (!ctlLive(g_ctl[g_cursor])) moveCursor(1);
            }
        } else if (!g_bindHint) {
            if (st.down(pad::Left)  && !g_dest[2].set) puckX -= speed * dt;
            if (st.down(pad::Right) && !g_dest[3].set) puckX += speed * dt;
            if (st.down(pad::Up)    && !g_dest[0].set) puckY -= speed * dt;
            if (st.down(pad::Down)  && !g_dest[1].set) puckY += speed * dt;
        }
        puckX = puckX < 0 ? 0 : (puckX > 255 ? 255 : puckX);
        puckY = puckY < 0 ? 0 : (puckY > 255 ? 255 : puckY);
        g_params.x.store(int(puckX + 0.5f));
        g_params.y.store(int(puckY + 0.5f));

        // hold a shoulder trigger to keep changing, faster the longer it is held
        const bool down = heldDir < 0 ? st.down(pad::L2) : (heldDir > 0 && st.down(pad::R2));
        if (!down) { heldDir = 0; held = 0; }
        else {
            held += dt;
            if (held > 0.28) {
                const int mult = held > 1.6 ? 6 : (held > 0.9 ? 3 : 1);
                static double acc = 0;
                acc += dt;
                while (acc > 0.04) { nudge(heldDir, mult); acc -= 0.04; }
            }
        }

        draw(win.renderer, out, dt);
        win.present();
        SDL_Delay(8);
    }

    g_params.playing.store(false);
    out.close();
    return 0;   // in a sndstation session, the launcher comes back
}
