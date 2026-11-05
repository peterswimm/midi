// test_blend.cpp — the blend must never invent a byte.
//
// The claim the Dither model makes is narrow and checkable: every byte it
// emits was produced by one of the four corner programs at that same tick,
// and each corner wins ticks in proportion to its bilinear weight. A future
// "improvement" that smooths the crossfade will break the first assertion;
// one that reaches for a random number instead of the equidistributed
// sequence will break the second.
#include "engine.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

using namespace gritty;

static int fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++fails; }
}

int main() {
    // A position with four unequal, all-significant weights.
    const Corners c = cornersAt(3 * 64 + 20, 1 * 64 + 48);
    int seen[4] = {0,0,0,0};
    bool authentic = true, differs = false;

    for (int64_t t = 0; t < 200000; ++t) {
        const int b = int(blendByte(c, t, int(Blend::Dither)));
        int from = -1;
        for (int k = 0; k < 4; ++k) if (sample(c.idx[k], t) == b) { from = k; break; }
        if (from < 0) authentic = false;                 // a byte nobody produced
        seen[blendPick(c, t)]++;
        const int mean = int(std::lround(double(
            c.w[0]*sample(c.idx[0],t) + c.w[1]*sample(c.idx[1],t) +
            c.w[2]*sample(c.idx[2],t) + c.w[3]*sample(c.idx[3],t))));
        if (std::abs(mean - b) > 1) differs = true;      // and not the old average
    }
    check(authentic, "every dithered byte comes from a corner program");
    check(differs,   "the dithered byte is not just the weighted average");

    // Each corner's share of the ticks is its weight.
    double worst = 0;
    for (int k = 0; k < 4; ++k) {
        const double got = double(seen[k]) / 200000.0;
        const double want = double(c.w[k]);
        if (want > BLEND_EPS && std::fabs(got - want) > worst) worst = std::fabs(got - want);
    }
    check(worst < 0.002, "each corner wins ticks in proportion to its weight");

    // Standing on a node, the blend is that one formula and nothing else.
    const Corners on = cornersAt(0, 0);
    bool pure = true;
    for (int64_t t = 0; t < 20000; ++t)
        if (int(blendByte(on, t, int(Blend::Dither))) != sample(0, t)) pure = false;
    check(pure, "on a node the blend is that formula exactly");

    // Nearest is always a real byte too; Mean is still the old average.
    bool nearOk = true, meanOk = true;
    const Corners mid = cornersAt(2 * 64 + 32, 2 * 64 + 32);
    for (int64_t t = 0; t < 20000; ++t) {
        const int nb = int(blendByte(mid, t, int(Blend::Nearest)));
        bool found = false;
        for (int k = 0; k < 4; ++k) if (sample(mid.idx[k], t) == nb) found = true;
        if (!found) nearOk = false;
        float m = 0;
        for (int k = 0; k < 4; ++k) m += mid.w[k] * float(sample(mid.idx[k], t));
        if (std::fabs(m - blendByte(mid, t, int(Blend::Mean))) > 1e-4f) meanOk = false;
    }
    check(nearOk, "nearest emits a real byte");
    check(meanOk, "mean is unchanged");

    if (fails) return 1;
    std::printf("test_blend: 200000 ticks, every byte authentic, shares %.3f/%.3f/%.3f/%.3f "
                "within %.4f of the weights\n",
                seen[0]/200000.0, seen[1]/200000.0, seen[2]/200000.0, seen[3]/200000.0, worst);
    return 0;
}
