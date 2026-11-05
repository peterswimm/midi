// Gritty Grids for sndstation — a bytebeat drum machine for Linux handhelds.
//
// A native port of ../demos/gritty-grids.html: the 25-node Grids map, the
// bilinear blend, both engines, the polyrhythm, A/R/P and Chaos, driven by a
// gamepad on a 640x480 panel instead of a mouse on a web page.
//
//   gritty-grids               run it
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

// ── the phosphor screen, lifted from the demo's CSS ──
constexpr SDL_Color PHOS   {0x7D, 0xFB, 0xA0, 255};
constexpr SDL_Color PHOS_D {0x39, 0xC4, 0x6E, 255};
constexpr SDL_Color AMBER  {0xFF, 0xB3, 0x47, 255};
constexpr SDL_Color SCREEN {0x06, 0x0D, 0x0A, 255};

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

Ctl  g_ctl[2 + 3 * 5];
int  g_nCtl = 0;
int  g_cursor = 0;

void buildControls() {
    g_nCtl = 0;
    g_ctl[g_nCtl++] = {"BPM",   -1, &g_params.bpm,   40, 240, 1};
    g_ctl[g_nCtl++] = {"CHAOS", -1, &g_params.chaos,  0, 200, 2};
    for (int i = 0; i < 3; ++i) {
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
    const float c0[3] = {6, 13, 10}, c1[3] = {31, 107, 63}, c2[3] = {255, 179, 71};
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
    gfx::fillRect(r, dst, SCREEN);
    if (g_field) gfx::blit(r, g_field, nullptr, &dst);
    gfx::frameRect(r, dst, line);

    // the 25 nodes
    for (int gy = 0; gy < 5; ++gy)
        for (int gx = 0; gx < 5; ++gx)
            gfx::disc(r, float(bx) + gx / 4.0f * size, float(by) + gy / 4.0f * size,
                      suf(1.8f), withAlpha(PHOS, 90));

    // the four that are actually sounding, sized by weight
    for (int k = 0; k < 4; ++k) {
        if (c.w[k] < 0.02f) continue;
        const int n = c.idx[k];
        const float gx = float(n % 5), gy = float(n / 5);
        gfx::disc(r, float(bx) + gx / 4.0f * size, float(by) + gy / 4.0f * size,
                  suf(2.0f + 4.0f * c.w[k]), withAlpha(AMBER, Uint8(80 + 175 * c.w[k])));
    }

    // crosshair + puck
    const float px = float(bx) + g_params.x.load() / 255.0f * size;
    const float py = float(by) + g_params.y.load() / 255.0f * size;
    gfx::fillRect(r, {int(px), by, 1, size}, withAlpha(PHOS, 70));
    gfx::fillRect(r, {bx, int(py), size, 1}, withAlpha(PHOS, 70));
    gfx::glow(r, px, py, suf(13), withAlpha(PHOS, 110));
    gfx::disc(r, px, py, suf(7), PHOS);
    gfx::disc(r, px, py, suf(2.6f), SCREEN);
}

void drawBlend(SDL_Renderer* r, int bx, int by, int w) {
    using namespace theme;
    const int X = g_params.x.load(), Y = g_params.y.load();
    const Corners c = cornersAt(X, Y);
    const int dom = domNode(X, Y);
    const Node* N = nodes();

    char hdr[64];
    std::snprintf(hdr, sizeof hdr, "node %d \xC2\xB7 %s", dom, N[dom].author);
    gfx::textMid(r, Font::Small, hdr, bx, by + su(6), txt2);
    gfx::textMid(r, Font::Label, N[dom].expr, bx, by + su(24), PHOS);

    char coord[32];
    std::snprintf(coord, sizeof coord, "X %d \xC2\xB7 Y %d", X, Y);
    gfx::textMid(r, Font::Small, coord, bx + w, by + su(6), AMBER, 1);

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
        gfx::textMid(r, Font::Small, tag, bx, y + su(7), isDom ? AMBER : txt2);
        gfx::textMid(r, Font::Small, N[ni].expr, bx + su(26), y + su(7),
                     isDom ? txt0 : txt1);
        char p[8];
        std::snprintf(p, sizeof p, "%d%%", pct);
        gfx::textMid(r, Font::Small, p, bx + w, y + su(7), isDom ? AMBER : txt1, 1);
        const int barW = w - su(26);
        gfx::fillRect(r, {bx + su(26), y + su(16), barW, su(3)}, bg2);
        gfx::fillRect(r, {bx + su(26), y + su(16), barW * pct / 100, su(3)},
                      isDom ? AMBER : PHOS_D);
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
                     g_ctl[g_cursor].lane == vi ? PHOS : txt1);
        gfx::textMid(r, Font::Small, laneRole(vi, mode), bx, y + su(21), txt2);

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
        for (int s = 0; s < Ln; ++s) {
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
            SDL_Color fill = bg1;
            if (cand) fill = bg2;
            if (on)   fill = (L > 212.0f) ? AMBER : PHOS_D;
            if (g_flash[vi].t > 0 && g_flash[vi].idx == s) fill = PHOS;
            gfx::fillRect(r, cell, fill);
            if (beat) gfx::fillRect(r, {cell.x, cell.y, su(2), cell.h}, withAlpha(PHOS, 60));
            gfx::frameRect(r, cell, s == ph ? PHOS : line);
        }

        // Bit voice: the lane's own 1-bit trace, to the right of its steps.
        if (mode == Mode::Voice) {
            const int sx = cellsX + cellsW + su(8), sw = su(80), sh = su(18);
            gfx::fillRect(r, {sx, y + su(4), sw, sh}, SCREEN);
            const int start = g_meters.scopePtr[vi].load();
            for (int j = 0; j < sw; ++j) {
                const int   si = (start + j * SCOPE_N / sw) & (SCOPE_N - 1);
                const float v  = g_meters.scope[vi][si];
                const int   yy = y + su(4) + int(v > 0 ? sh * 0.26f : sh * 0.74f);
                gfx::fillRect(r, {sx + j, yy, 1, su(2)}, PHOS);
            }
            gfx::frameRect(r, {sx, y + su(4), sw, sh}, line);
        }
    }
}

void drawStrip(SDL_Renderer* r, int bx, int by, int w) {
    using namespace theme;
    const int focusLane = g_ctl[g_cursor].lane;
    gfx::fillRect(r, {bx, by, w, su(26)}, bg1);

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
        gfx::fillRect(r, {x, by + su(3), tw, su(20)}, sel ? PHOS_D : bg2);
        gfx::textMid(r, Font::Small, label, x + tw / 2, by + su(13), sel ? SCREEN : txt1, 0);
        x += tw + su(5);
    }
    if (focusLane >= 0) {
        gfx::textMid(r, Font::Small, laneName(focusLane), bx + w - su(6), by + su(13), AMBER, 1);
    }
}

void draw(SDL_Renderer* r, const AudioOut& out, float dt) {
    using namespace theme;
    gfx::clear(r, bg0);

    const Mode   mode    = Mode(g_params.mode.load());
    const Rhythm rhythm  = Rhythm(g_params.rhythm.load());
    const bool   running = g_params.playing.load();

    // HUD
    gfx::fillRect(r, {0, 0, SCREEN_W, HUD_H}, bg1);
    gfx::textMid(r, Font::Title, "GRITTY GRIDS", su(16), HUD_H / 2, txt0);
    int hx = su(16) + gfx::textWidth(Font::Title, "GRITTY GRIDS") + su(14);
    const char* pills[2] = {mode == Mode::Voice ? "BIT VOICE" : "BITSEQ",
                            rhythm == Rhythm::Formula ? "FORMULA" : "ROLES"};
    for (const char* p : pills) {
        const int tw = gfx::textWidth(Font::Small, p) + su(14);
        gfx::fillRect(r, {hx, HUD_H / 2 - su(9), tw, su(18)}, bg2);
        gfx::textMid(r, Font::Small, p, hx + tw / 2, HUD_H / 2, PHOS, 0);
        hx += tw + su(6);
    }
    const char* tr = running ? "RUN" : "STOP";
    const int trw = gfx::textWidth(Font::Small, tr) + su(16);
    gfx::fillRect(r, {SCREEN_W - su(16) - trw, HUD_H / 2 - su(9), trw, su(18)},
                  running ? PHOS_D : bg2);
    gfx::textMid(r, Font::Small, tr, SCREEN_W - su(16) - trw / 2, HUD_H / 2,
                 running ? SCREEN : txt2, 0);
    gfx::hline(r, 0, HUD_H, SCREEN_W, line);

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
    gfx::fillRect(r, {0, fy, SCREEN_W, FOOTER_H}, bg1);
    std::string hint = std::string(pad::label(pad::East)) + " run   " +
                       pad::label(pad::South) + " engine   " +
                       pad::label(pad::North) + " rhythm   " +
                       pad::label(pad::West) + " lane   L1/R1 pick   L2/R2 set   " +
                       pad::label(pad::Select) + " reset   " +
                       pad::label(pad::Start) + " quit";
    gfx::textMid(r, Font::Small, hint, su(16), fy + FOOTER_H / 2, txt1);
    if (out.deviceOpen()) {
        char a[48];
        std::snprintf(a, sizeof a, "%d Hz \xC2\xB7 %d", out.sampleRate(), out.deviceFrames());
        gfx::textMid(r, Font::Small, a, SCREEN_W - su(16), fy + FOOTER_H / 2, txt2, 1);
    } else {
        gfx::textMid(r, Font::Small, "no audio device", SCREEN_W - su(16),
                     fy + FOOTER_H / 2, warn, 1);
    }
}

// ── headless: render audio with no device, and say what it cost ──
int bench(double seconds) {
    const int sr = 48000, block = 256;
    g_engine.setRate(sr);
    static float buf[block * 2];
    const long blocks = long(seconds * sr / block);
    int worst = 0;
    for (int mode = 0; mode < 2; ++mode) {
        g_params.mode.store(mode);
        g_params.playing.store(true);
        g_engine.rewind();
        const auto t0 = std::chrono::steady_clock::now();
        double peak = 0, sumSq = 0;
        long   clipped = 0, total = 0;
        for (long b = 0; b < blocks; ++b) {
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
                    "peak %.2f  rms %.3f  clipped %.3f%%\n",
                    mode ? "bit-voice" : "bitseq", ms, seconds, ms / realtime * 100.0,
                    peak, std::sqrt(sumSq / double(total)),
                    double(clipped) / double(total) * 100.0);
        if (peak < 0.01) { std::printf("  silent: that is a bug\n"); worst = 1; }
    }
    g_params.playing.store(false);
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
        else if (!std::strcmp(argv[i], "--res")   && i + 1 < argc)
            std::sscanf(argv[++i], "%dx%d", &wo.width, &wo.height);
    }
    buildControls();
    if (benchSec > 0) return bench(benchSec);

    wo.headless = !shots.empty();
    Window win;
    if (!win.open(wo)) return 1;
    buildField(win.renderer);

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
        struct Shot { const char* name; Mode m; Rhythm r; bool run; int cursor; };
        const Shot shots_[] = {
            {"bitseq-roles",    Mode::Bitseq, Rhythm::Roles,   false, 0},
            {"bitseq-formula",  Mode::Bitseq, Rhythm::Formula, true,  3},
            {"voice-roles",     Mode::Voice,  Rhythm::Roles,   true,  6},
            {"voice-formula",   Mode::Voice,  Rhythm::Formula, true,  11},
        };
        int bad = 0;
        for (const Shot& s : shots_) {
            g_params.mode.store(int(s.m));
            g_params.rhythm.store(int(s.r));
            g_params.playing.store(s.run);
            g_params.x.store(s.run ? 196 : 128);
            g_params.y.store(s.run ? 72  : 128);
            g_cursor = s.cursor;
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
                    case pad::Select:                             // back to the defaults
                        g_params.reset();
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

        // the puck: stick is a velocity, D-pad is a step
        const float speed = 150.0f;    // units per second at full deflection
        puckX += st.lx * speed * dt;
        puckY -= st.ly * speed * dt;   // stick y is positive up; the map is not
        if (st.down(pad::Left))  puckX -= speed * dt;
        if (st.down(pad::Right)) puckX += speed * dt;
        if (st.down(pad::Up))    puckY -= speed * dt;
        if (st.down(pad::Down))  puckY += speed * dt;
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
