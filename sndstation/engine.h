// engine.h — Gritty Grids' DSP, as one audio-thread render callback.
//
// The web demo runs on two clocks: WebAudio's scheduler builds Bitseq hits as
// buffers a tenth of a second ahead, while Bit voice runs sample by sample in
// a ScriptProcessor. sndstation gives an app one render(float*, frames), so
// here both engines run sample-accurate inside it, and "schedule ahead"
// becomes a one-pulse lookahead queue — which is also the only way the web
// version's NEGATIVE timing jitter survives the port.
//
// Nothing in render() allocates, locks or calls into the standard library's
// RNG: a grain's wavetable lives in a fixed pool slot, and the one random
// source is an xorshift32 owned by the engine.
#pragma once
#include "bytebeat.h"
#include <atomic>
#include <cmath>
#include <cstdint>

namespace gritty {

constexpr int   STEPS         = 16;
constexpr int   STRIDE        = 311;
constexpr int   CBASE         = 20000;
constexpr int   OFF[3]        = {0, 1013, 3301};
constexpr float BYTE_RATE     = 8000.0f;
constexpr float BASE_MUL[3]   = {0.5f, 1.0f, 2.0f};        // Bit voice: base octave per lane
constexpr int   PCYCLE        = 720720;                    // LCM(1..16): aligns to any track length
constexpr int   LEN_DEF[3]    = {16, 12, 9};               // polyrhythm: phase over 144 pulses
constexpr float GRAIN_FREQ[3] = {55.0f, 190.0f, 320.0f};   // kick / snare / hat register
constexpr int   GRAIN_START[3]= {50000, 61000, 73000};     // where the wavetable is grabbed
constexpr int   VSTART[3]     = {50000, 61000, 73000};     // Bit voice: t starts lively
constexpr int   ATK_DEF[3]    = {8, 12, 4};                // bass punchy, hats tight
constexpr int   REL_DEF[3]    = {45, 30, 14};
constexpr int   PROB_DEF[3]   = {75, 75, 75};              // P75 = 100% chance, no roll
constexpr int   DENS_DEF[3]   = {180, 200, 140};
constexpr int   WT_N          = 256;
constexpr int   SCOPE_N       = 256;
constexpr int   MAX_GRAINS    = 24;

enum class Mode   { Bitseq, Voice };
enum class Rhythm { Roles, Formula };

inline const char* laneName(int i) {
    static const char* n[3] = {"FOUR", "BACKBEAT", "EIGHTHS"};
    return n[i];
}
inline const char* laneRole(int i, Mode m) {
    static const char* seq[3] = {"on the floor", "2 & 4", "off-eighths"};
    static const char* voc[3] = {"1-bit \xC2\xB7 bass", "1-bit \xC2\xB7 mid", "1-bit \xC2\xB7 treble"};
    return m == Mode::Voice ? voc[i] : seq[i];
}
// The rhythmic ROLE a lane plays in Roles mode: which steps it may fire on.
inline bool candidate(int lane, int s) {
    switch (lane) {
        case 0:  return s % 4 == 0;     // four on the floor
        case 1:  return s % 8 == 4;     // backbeat, 2 & 4
        default: return s % 2 == 0;     // eighths
    }
}

// ── the 16-step curve each node draws for each lane ──
// Computed once: the node set never changes, so neither does this.
struct Curves {
    uint8_t v[25][3][STEPS];
    float   scalar[25];                 // average energy, for the heat field
    Curves() {
        for (int n = 0; n < 25; ++n) {
            float sum = 0;
            for (int vc = 0; vc < 3; ++vc)
                for (int s = 0; s < STEPS; ++s) {
                    const int x = sample(n, CBASE + int64_t(s) * STRIDE + OFF[vc]);
                    v[n][vc][s] = uint8_t(x);
                    sum += float(x);
                }
            scalar[n] = sum / (3.0f * STEPS * 255.0f);
        }
    }
};
inline const Curves& curves() { static const Curves c; return c; }

// ── the Grids-style bilinear blend of the four nearest map nodes ──
struct Corners {
    int   idx[4];
    float w[4];
    int   xi, yi;
    float fx, fy;
};
inline Corners cornersAt(int X, int Y) {
    Corners c{};
    c.xi = X >> 6; if (c.xi > 3) c.xi = 3;
    c.yi = Y >> 6; if (c.yi > 3) c.yi = 3;
    c.fx = float(X - (c.xi << 6)) / 64.0f; c.fx = c.fx < 0 ? 0 : (c.fx > 1 ? 1 : c.fx);
    c.fy = float(Y - (c.yi << 6)) / 64.0f; c.fy = c.fy < 0 ? 0 : (c.fy > 1 ? 1 : c.fy);
    const int x1 = c.xi + 1 > 4 ? 4 : c.xi + 1;
    const int y1 = c.yi + 1 > 4 ? 4 : c.yi + 1;
    c.idx[0] = c.yi * 5 + c.xi; c.idx[1] = c.yi * 5 + x1;
    c.idx[2] = y1   * 5 + c.xi; c.idx[3] = y1   * 5 + x1;
    c.w[0] = (1 - c.fx) * (1 - c.fy); c.w[1] = c.fx * (1 - c.fy);
    c.w[2] = (1 - c.fx) * c.fy;       c.w[3] = c.fx * c.fy;
    return c;
}
inline float levelAt(const Corners& c, int lane, int step) {
    const Curves& cv = curves();
    float L = 0;
    for (int k = 0; k < 4; ++k) L += c.w[k] * float(cv.v[c.idx[k]][lane][step]);
    return L;
}
// The 16-step curve resampled to an arbitrary phase, so a track can be any
// length and the three tracks can phase against each other.
inline float grooveLevel(const Corners& c, int lane, float phase) {
    const float fp = phase * STEPS;
    const float fl = std::floor(fp);
    const int   s0 = int(fl) % STEPS, s1 = (s0 + 1) % STEPS;
    const float fr = fp - fl;
    return levelAt(c, lane, s0) * (1 - fr) + levelAt(c, lane, s1) * fr;
}
// Relative threshold: fire the top `dfrac` of THIS position's curve range,
// not an absolute level. Activity stays proportional everywhere, so there
// are no dead spots on the map and the blend is always audible. A flat curve
// has no rhythmic content, and stays silent.
inline float trackThreshold(const Corners& c, int lane, int Ln, float dfrac) {
    float mn = 1e9f, mx = -1e9f;
    for (int k = 0; k < Ln; ++k) {
        const float L = grooveLevel(c, lane, float(k) / float(Ln));
        if (L < mn) mn = L;
        if (L > mx) mx = L;
    }
    if (mx - mn < 1.0f) return mx + 1.0f;
    return mn + (1 - dfrac) * (mx - mn);
}
// The node nearest the puck, for the headline.
inline int domNode(int X, int Y) {
    int nx = int(std::lround(X / 255.0f * 4));
    int ny = int(std::lround(Y / 255.0f * 4));
    if (nx > 4) nx = 4;
    if (ny > 4) ny = 4;
    return ny * 5 + nx;
}

// ── what the UI sets and the audio thread reads ──
// Scalars only, each its own atomic. The audio thread takes one snapshot per
// block, so a control moved mid-block lands on the next one.
struct Params {
    std::atomic<int>  x{128}, y{128};              // map position, 0..255
    std::atomic<int>  dens[3];                     // density (Bitseq) / pitch (Voice), 0..255
    std::atomic<int>  len[3];                      // track length, 2..16
    std::atomic<int>  atk[3], rel[3], prob[3];     // 0..100
    std::atomic<int>  chaos{30};                   // 0..200
    std::atomic<int>  bpm{120};
    std::atomic<bool> playing{false};
    std::atomic<int>  mode{int(Mode::Bitseq)};
    std::atomic<int>  rhythm{int(Rhythm::Roles)};

    Params() { reset(); }
    void reset() {
        x = 128; y = 128; chaos = 30; bpm = 120;
        for (int i = 0; i < 3; ++i) {
            dens[i] = DENS_DEF[i]; len[i] = LEN_DEF[i];
            atk[i]  = ATK_DEF[i];  rel[i] = REL_DEF[i]; prob[i] = PROB_DEF[i];
        }
    }
    float envAtk(int i) const { const float u = atk[i] / 100.0f; return 0.001f + u * u * 0.30f; }
    float envRel(int i) const { const float u = rel[i] / 100.0f; return 0.02f  + u * u * 1.50f; }
    float voiceRate(int i) const { return BASE_MUL[i] * std::pow(2.0f, (dens[i] - 128) / 96.0f); }
    // P: 0..75 is trigger chance (0..100%); 75..100 holds 100% and rolls.
    float probChance(int i) const {
        const float t = prob[i] / 100.0f;
        return t <= 0.75f ? t / 0.75f : 1.0f;
    }
    int ratchetN(int i) const {
        const float t = prob[i] / 100.0f;
        if (t <= 0.75f) return 1;
        const int n = int(std::floor(std::pow(2.0f, (t - 0.75f) / 0.25f * 3.0f)));
        return n < 1 ? 1 : (n > 8 ? 8 : n);
    }
    int laneLen(int i) const {
        if (Rhythm(rhythm.load()) != Rhythm::Formula) return STEPS;
        const int L = len[i].load();
        return L < 2 ? 2 : (L > 16 ? 16 : L);
    }
};

// ── what the audio thread tells the UI ──
// Counters and last-values only. The scope is a plain float ring written
// while it is read: a torn scope trace is still a scope trace, and locking
// the audio thread to draw one would not be.
struct Meters {
    std::atomic<int>      pulse{0};
    std::atomic<int>      step[3];                 // the playhead, per lane
    std::atomic<uint32_t> hits[3];                 // bumped per hit, for the flash
    std::atomic<uint32_t> accents[3];
    std::atomic<float>    peak{0};
    float                 scope[3][SCOPE_N] = {};
    std::atomic<int>      scopePtr[3];

    Meters() {
        for (int i = 0; i < 3; ++i) { step[i] = 0; hits[i] = 0; accents[i] = 0; scopePtr[i] = 0; }
    }
};

class Engine {
public:
    Engine(Params& p, Meters& m) : P(p), M(m) {}

    void setRate(int sr) { m_sr = sr > 0 ? sr : 48000; }
    int  rate() const { return m_sr; }

    // A run always begins at pulse 0, the way hitting Run in the demo does.
    void rewind() {
        m_pulse = 0; m_stepClock = 0; m_nQueued = 0; m_samples = 0; m_warm = false;
        for (int i = 0; i < 3; ++i) {
            m_vAcc[i] = double(VSTART[i]); m_vTi[i] = -1; m_vVal[i] = 0;
            m_env[i] = 0; m_stage[i] = 0;
            m_rCount[i] = 0; m_rClock[i] = 0; m_rInt[i] = 0;
            M.step[i].store(0);
        }
        for (auto& g : m_grain) g.active = false;
        m_lp = m_dcX = m_dcY = 0;
    }

    // Audio thread. `out` is interleaved stereo and arrives silent.
    void render(float* out, int frames) {
        if (!P.playing.load()) { M.peak.store(0.0f); return; }
        const Mode    mode   = Mode(P.mode.load());
        const Rhythm  rhythm = Rhythm(P.rhythm.load());
        const Corners c      = cornersAt(P.x.load(), P.y.load());
        const double  spStep = double(m_sr) * (60.0 / double(P.bpm.load()) / 4.0);   // a 16th note
        const float   cch    = P.chaos.load() / 200.0f;
        float peak = 0;

        if (mode == Mode::Voice) renderVoice (out, frames, c, rhythm, spStep, cch, peak);
        else                     renderBitseq(out, frames, c, rhythm, spStep, cch, peak);

        M.peak.store(peak);
    }

private:
    // ── Bitseq: a hit is a short grain of the blended formula's WAVESHAPE ──
    // Not a 1-bit hash: the interpolated program becomes a 256-sample
    // wavetable played at the lane's own pitch, so the drum's timbre morphs
    // as the puck moves. The sound is literally the blended bytebeat.
    struct Grain {
        bool  active = false;
        int   lane = 0, n = 0, len = 0, atkN = 1;
        float f0 = 0, gain = 0, phase = 0;
        float wt[WT_N] = {};
    };
    struct Pending { int64_t due; int lane; bool accent; float mut; };

    void renderBitseq(float* out, int frames, const Corners& c, Rhythm rhythm,
                      double spStep, float cch, float& peak) {
        const float chaos = float(P.chaos.load());
        for (int n = 0; n < frames; ++n) {
            if (m_stepClock <= 0) {
                // Decide the NEXT pulse now and queue its hits one pulse out.
                // That lookahead is what lets chaos pull a hit EARLY as well
                // as late, the way the web demo's scheduler could.
                const int lp = (m_pulse + (m_warm ? 1 : 0)) % PCYCLE;
                for (int vi = 0; vi < 3; ++vi) {
                    const float dens = float(P.dens[vi].load());
                    float L;
                    bool  on;
                    if (rhythm == Rhythm::Formula) {
                        const int Ln = P.laneLen(vi);
                        L  = grooveLevel(c, vi, float(lp % Ln) / float(Ln));
                        on = L > trackThreshold(c, vi, Ln, dens / 255.0f);
                    } else {
                        const int s = lp % STEPS;
                        L  = levelAt(c, vi, s);
                        on = candidate(vi, s) && L > 255.0f - dens;
                    }
                    L += (rnd() * 2 - 1) * chaos;                    // chaos -> pattern
                    if (!on) continue;
                    const double jit = double((rnd() * 2 - 1) * cch * 0.45f) * spStep;
                    double due = (m_warm ? spStep : 0.0) + jit;      // chaos -> timing
                    if (due < 0) due = 0;
                    queue(m_samples + int64_t(due), vi, L > 212.0f, cch);
                }
                m_pulse = (m_pulse + 1) % PCYCLE;
                m_warm  = true;
                m_stepClock += spStep;
                publishPulse();
            }
            m_stepClock -= 1.0;

            fireDue(c);

            float mix = 0;
            for (auto& g : m_grain) {
                if (!g.active) continue;
                const float prog = float(g.n) / float(g.len);
                // The kick drops in pitch across the grain; the others hold.
                const float f = g.f0 * (g.lane == 0 ? (1 + 1.1f * std::pow(1 - prog, 3.0f)) : 1.0f);
                g.phase += f * WT_N / float(m_sr);
                float ip = std::fmod(g.phase, float(WT_N));
                if (ip < 0) ip += WT_N;
                const int   i0 = int(ip), i1 = (i0 + 1) % WT_N;
                const float fr = ip - float(i0);
                const float w  = g.wt[i0] * (1 - fr) + g.wt[i1] * fr;
                float env;
                if (g.n < g.atkN) {
                    env = float(g.n) / float(g.atkN);
                } else {
                    const int   tail = g.len - g.atkN > 1 ? g.len - g.atkN : 1;
                    const float p    = float(g.n - g.atkN) / float(tail);
                    env = std::pow(1 - p, g.lane == 0 ? 1.5f : 2.4f);
                }
                mix += w * env * g.gain;
                if (++g.n >= g.len) g.active = false;
            }
            const float s = clip(mix * 0.85f);                       // the demo's master gain
            out[2 * n] = out[2 * n + 1] = s;
            const float a = std::fabs(s);
            if (a > peak) peak = a;
            ++m_samples;
        }
    }

    // ── Bit voice: three 1-bit bytebeat oscillators ──
    // Each lane blends the four corner formulas, takes the top bit each
    // byte-clock tick, and gates the resulting square with its own AR: Grids'
    // three trigger outs repurposed as three voices, one pin at a time.
    void renderVoice(float* out, int frames, const Corners& c, Rhythm rhythm,
                     double spStep, float cch, float& peak) {
        const float chaos = float(P.chaos.load());
        const float grit  = cch * 0.55f;
        double inc[3];
        float  aInc[3], rInc[3];
        for (int i = 0; i < 3; ++i) {
            inc[i]  = double(P.voiceRate(i)) * double(BYTE_RATE) / double(m_sr);
            aInc[i] = 1.0f / (P.envAtk(i) * float(m_sr));
            rInc[i] = 1.0f / (P.envRel(i) * float(m_sr));
        }
        for (int n = 0; n < frames; ++n) {
            if (m_stepClock <= 0) {                 // pulse boundary -> retrigger
                for (int i = 0; i < 3; ++i) {
                    bool hit;
                    if (rhythm == Rhythm::Formula) {
                        const int Ln = P.laneLen(i);
                        hit = (grooveLevel(c, i, float(m_pulse % Ln) / float(Ln))
                               + (rnd() * 2 - 1) * chaos)
                              > trackThreshold(c, i, Ln, 0.5f);      // chaos -> pattern
                    } else {
                        hit = candidate(i, m_pulse % STEPS);
                    }
                    if (hit && rnd() < P.probChance(i)) {
                        const int rn = P.ratchetN(i);
                        m_rCount[i] = rn;
                        m_rInt[i]   = spStep / rn;
                        m_rClock[i] = double(cch * rnd() * 0.45f) * spStep;   // chaos -> timing
                        M.hits[i].fetch_add(1);
                    }
                }
                m_pulse = (m_pulse + 1) % PCYCLE;
                m_stepClock += spStep;
                publishPulse();
            }
            m_stepClock -= 1.0;

            float mix = 0;
            for (int i = 0; i < 3; ++i) {
                if (m_rCount[i] > 0) {                               // ratchet / roll
                    if (m_rClock[i] <= 0) {
                        m_stage[i] = 1;
                        --m_rCount[i];
                        m_rClock[i] += m_rInt[i];
                    }
                    m_rClock[i] -= 1.0;
                }
                m_vAcc[i] += inc[i];
                const int64_t ti = int64_t(std::floor(m_vAcc[i]));
                if (ti != m_vTi[i]) {
                    m_vTi[i] = ti;
                    float val = 0;
                    for (int k = 0; k < 4; ++k) val += c.w[k] * float(sample(c.idx[k], ti));
                    m_vVal[i] = val;
                }
                if      (m_stage[i] == 1) { m_env[i] += aInc[i]; if (m_env[i] >= 1) { m_env[i] = 1; m_stage[i] = 2; } }
                else if (m_stage[i] == 2) { m_env[i] -= rInc[i]; if (m_env[i] <= 0) { m_env[i] = 0; m_stage[i] = 0; } }
                float bit = (int(m_vVal[i]) & 128) ? 1.0f : -1.0f;
                if (grit > 0 && rnd() < grit) bit = -bit;            // chaos -> grit
                const float smp = bit * m_env[i];
                mix += smp;
                if ((n & 7) == 0) {
                    const int p = M.scopePtr[i].load(std::memory_order_relaxed);
                    M.scope[i][p] = smp;
                    M.scopePtr[i].store((p + 1) & (SCOPE_N - 1), std::memory_order_relaxed);
                }
            }
            const float s = mix * 0.5f;
            m_lp += 0.6f * (s - m_lp);                               // tames 1-bit aliasing
            const float dc = m_lp - m_dcX + 0.995f * m_dcY;          // centre the duty cycle
            m_dcX = m_lp;
            m_dcY = dc;
            const float o = clip(dc);
            out[2 * n] = out[2 * n + 1] = o;
            const float a = std::fabs(o);
            if (a > peak) peak = a;
            ++m_samples;
        }
    }

    void publishPulse() {
        M.pulse.store(m_pulse);
        for (int vi = 0; vi < 3; ++vi) M.step[vi].store(m_pulse % P.laneLen(vi));
    }

    void queue(int64_t due, int lane, bool accent, float mut) {
        if (m_nQueued >= int(sizeof(m_queue) / sizeof(m_queue[0]))) return;
        m_queue[m_nQueued++] = {due, lane, accent, mut};
    }
    void fireDue(const Corners& c) {
        for (int i = 0; i < m_nQueued;) {
            if (m_queue[i].due > m_samples) { ++i; continue; }
            const Pending q = m_queue[i];
            m_queue[i] = m_queue[--m_nQueued];
            grainHit(c, q.lane, q.accent, q.mut);
            M.hits[q.lane].fetch_add(1);
            if (q.accent) M.accents[q.lane].fetch_add(1);
        }
    }

    void grainHit(const Corners& c, int lane, bool accent, float mut) {
        Grain* g = nullptr;
        for (auto& cand : m_grain)
            if (!cand.active) { g = &cand; break; }
        if (!g) { g = &m_grain[m_steal]; m_steal = (m_steal + 1) % MAX_GRAINS; }

        int len = int(P.envRel(lane) * float(m_sr));                 // R = grain length
        if (len < 64) len = 64;
        // Chaos moves where in the formula the wavetable is grabbed, so each
        // hit's timbre mutates as well as its time.
        const int64_t wtStart = GRAIN_START[lane] + (mut > 0 ? int64_t(rnd() * mut * 8000.0f) : 0);
        float mean = 0;
        for (int j = 0; j < WT_N; ++j) {
            float v = 0;
            for (int k = 0; k < 4; ++k) v += c.w[k] * float(sample(c.idx[k], wtStart + j));
            g->wt[j] = v;
            mean += v;
        }
        mean /= float(WT_N);
        float pk = 1e-6f;
        for (int j = 0; j < WT_N; ++j) {
            g->wt[j] -= mean;                                        // DC removed
            const float a = std::fabs(g->wt[j]);
            if (a > pk) pk = a;
        }
        for (int j = 0; j < WT_N; ++j) g->wt[j] /= pk;               // normalized

        int atkN = int(P.envAtk(lane) * float(m_sr));                // A = attack
        if (atkN < 1) atkN = 1;
        if (atkN > len - 1) atkN = len - 1;
        g->lane = lane; g->len = len; g->atkN = atkN; g->n = 0; g->phase = 0;
        g->f0   = GRAIN_FREQ[lane] * (1 + (rnd() * 2 - 1) * mut * 0.05f);
        g->gain = accent ? 0.95f : 0.62f;
        g->active = true;
    }

    static float clip(float v) { return v < -1 ? -1 : (v > 1 ? 1 : v); }
    // xorshift32: the audio thread has no business calling a general RNG.
    float rnd() {
        m_rng ^= m_rng << 13;
        m_rng ^= m_rng >> 17;
        m_rng ^= m_rng << 5;
        return float(m_rng >> 8) * (1.0f / 16777216.0f);
    }

    Params&  P;
    Meters&  M;
    int      m_sr = 48000;
    int      m_pulse = 0;
    double   m_stepClock = 0;
    int64_t  m_samples = 0;
    bool     m_warm = false;
    uint32_t m_rng = 0x1234567u;

    Grain    m_grain[MAX_GRAINS];
    int      m_steal = 0;
    Pending  m_queue[64];
    int      m_nQueued = 0;

    double   m_vAcc[3] = {};
    int64_t  m_vTi[3]  = {};
    float    m_vVal[3] = {};
    float    m_env[3]  = {};
    int      m_stage[3] = {};
    int      m_rCount[3] = {};
    double   m_rClock[3] = {}, m_rInt[3] = {};
    float    m_lp = 0, m_dcX = 0, m_dcY = 0;
};

} // namespace gritty
