// test_free.cpp — the free lane must be the formula, not a version of it.
//
// A free lane exists so the one-liner's own music comes out intact. Every
// other path in this engine shapes what it touches: the grain rebuilds the
// signal as a wavetable, the voice path takes one bit of it and then runs a
// low-pass and a DC blocker over the result, and both end at a hard clip.
// None of that may happen here.
//
// So this test does not inspect the code, which can be rewritten around an
// assertion. It renders audio through the real Engine and compares it with
// the bytebeat computed directly from bytebeat.h, sample for sample. If
// anyone adds a filter, a limiter, a soft clip, a dither, a DC trim or an
// envelope to the free path, these numbers stop matching.
#include "engine.h"
#include <cmath>
#include <cstdio>

using namespace gritty;

int main() {
    const int SR = 44100, N = 4096;

    Params P;
    Meters M;
    Engine e{P, M};
    e.setRate(SR);

    // Stand exactly on node 0 so the blend weights are 1 and 0: what comes
    // out should be that one-liner and nothing else.
    P.x.store(0); P.y.store(0);
    P.bpm.store(120);
    P.chaos.store(0);
    for (int i = 0; i < 3; ++i) { P.freeRun[i].store(1); P.key[i].store(0); }
    P.playing.store(true);
    e.rewind();

    static float out[2 * N];
    e.render(out, N);

    // The same thing, computed straight from the formulas: three accumulators
    // starting where rewind puts them, a zero-order hold to the device rate,
    // the byte centred, and a plain average of the three.
    const double inc = freeBaseRate(120) / double(SR);
    double  acc[3] = {double(VSTART[0]), double(VSTART[1]), double(VSTART[2])};
    int64_t ti[3]  = {-1, -1, -1};
    float   val[3] = {0, 0, 0};

    double worst = 0;
    int    bad   = 0;
    for (int n = 0; n < N; ++n) {
        float want = 0;
        for (int i = 0; i < 3; ++i) {
            acc[i] += inc;
            const int64_t t = int64_t(std::floor(acc[i]));
            if (t != ti[i]) { ti[i] = t; val[i] = float(sample(0, t)); }
            want += (val[i] - 127.5f) * (1.0f / 127.5f);
        }
        want /= 3.0f;
        const double d = std::fabs(double(out[2 * n]) - double(want));
        if (d > worst) worst = d;
        if (d > 1e-6) ++bad;
        if (out[2 * n] != out[2 * n + 1]) {
            std::printf("FAIL: channels differ at %d\n", n);
            return 1;
        }
    }
    if (bad) {
        std::printf("FAIL: %d of %d free samples are not the formula "
                    "(worst %.9f) — something is processing the free path\n",
                    bad, N, worst);
        return 1;
    }

    // And it must not be silent, or the comparison above proves nothing.
    double energy = 0;
    for (int n = 0; n < N; ++n) energy += double(out[2 * n]) * double(out[2 * n]);
    if (std::sqrt(energy / N) < 0.01) {
        std::printf("FAIL: the free lane rendered silence\n");
        return 1;
    }

    // The quantised clock: a power-of-two block of t per beat.
    const double beat = freeBaseRate(120) / (120.0 / 60.0);
    const double lg   = std::log2(beat);
    if (std::fabs(lg - std::floor(lg + 0.5)) > 1e-9) {
        std::printf("FAIL: %.3f samples a beat is not a power of two\n", beat);
        return 1;
    }

    std::printf("test_free: %d samples bit-exact with the formula, "
                "%.0f (2^%d) per beat, rms %.3f\n",
                N, beat, int(std::lround(lg)), std::sqrt(energy / N));
    return 0;
}
