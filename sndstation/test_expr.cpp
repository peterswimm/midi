// test_expr.cpp — the evaluator against the hand-ported formulas.
//
// bytebeat.h carries 25 one-liners twice: once as their original JavaScript
// text, and once as C++ somebody wrote out by hand with the coercions spelled
// explicitly. expr.h is a third rendering, which reads the text directly. So
// the text and the hand port are two independent witnesses, and this test
// asks them to agree with it across a long sweep.
//
// That makes it a real check rather than a restatement: expr.h did not come
// from the C++ port and the C++ port did not come from expr.h, so a bug in
// either shows up as a disagreement. tools/import-library.py does the same
// job against node for the imported library, which is the other half.
#include "expr.h"
#include "bytebeat.h"
#include <cstdio>
#include <cmath>

using namespace gritty;

static int fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++fails; }
}
static double ev(const char* src) {
    Expr e;
    if (!e.compile(src)) { std::fprintf(stderr, "FAIL: %s -> %s\n", src, e.error()); ++fails; return -1e9; }
    return e.eval(0);
}
static void same(const char* src, double want) {
    const double got = ev(src);
    if (!(got == want || (std::isnan(got) && std::isnan(want)))) {
        std::fprintf(stderr, "FAIL: %s gave %.17g, wanted %.17g\n", src, got, want);
        ++fails;
    }
}

int main() {
    // ── the 25, text against hand port ──
    int checked = 0;
    for (int n = 0; n < 25; ++n) {
        Expr e;
        if (!e.compile(nodes()[n].expr)) {
            std::fprintf(stderr, "FAIL: node %d will not compile: %s\n", n, e.error());
            ++fails; continue;
        }
        bool ok = true;
        for (int64_t t = 0; t <= 300000; t += 13) {
            const int want = sample(n, t);
            const int got  = int(exprByte(e, MODE_BYTE, double(t)));
            if (want != got) {
                std::fprintf(stderr, "FAIL: node %d at t=%lld: hand port %d, evaluator %d\n",
                             n, (long long)t, want, got);
                ok = false; break;
            }
        }
        if (ok) ++checked;
    }
    check(checked == 25, "all 25 formulas agree with the hand port");

    // ── the coercions that are easy to get wrong ──
    same("1/0",               HUGE_VAL);          // a double divide, not integer
    same("(1/0)|0",           0);                 // ToInt32(Infinity) is 0
    same("-1>>>0",            4294967295.0);      // unsigned shift
    same("-1>>0",             -1);
    same("1<<33",             2);                 // the count is masked to 5 bits
    same("2**10",             1024);
    same("2**3**2",           512);               // right-associative
    same("-7%3",              -1);                // the sign of the dividend
    same("0x8898a989>>1",     -1001630524);       // the hex is a NEGATIVE int32
    same("5&&7",              7);                 // returns an operand, not a bool
    same("0&&7",              0);
    same("0||9",              9);
    same("~5",                -6);
    same("!0",                1);
    same("3<4",               1);

    // a one-character string is its VALUE under [], its CODE under charCodeAt
    same("\"6689\"[1]",             6);
    same("\"6689\".charCodeAt(1)",  54);
    same("[7,8,9][2]",              9);
    check(std::isnan(ev("[7,8,9][5]")), "a lookup past the end is NaN");

    // variables, in a frame that starts fresh every sample
    same("(t=10,t*2)",        20);
    same("(a=3,a+=4,a)",      7);
    same("(t=8,t/=4,t)",      2);
    check(std::isnan(ev("(q+1)")), "a variable read before it is written is NaN");

    // `random()` is reproducible: the same sample index gives the same noise
    // every pass, which is what lets it be verified at all. The values below
    // are what node produces under the same hash (tools/verify-expr.mjs).
    {
        Expr r;
        check(r.compile("random()*255"), "random compiles");
        check(int(exprByte(r, MODE_BYTE, 1)) == 80 &&
              int(exprByte(r, MODE_BYTE, 2)) == 48 &&
              int(exprByte(r, MODE_BYTE, 3)) == 133, "random matches javascript");
        check(exprByte(r, MODE_BYTE, 7) == exprByte(r, MODE_BYTE, 7),
              "random is the same on a second pass");
        // two calls in one sample are different draws, not the same one
        Expr r2;
        check(r2.compile("random()-random()"), "two draws compile");
        check(r2.eval(5) != 0.0, "two draws in a sample differ");
    }

    // arrow functions, which is how a song carries a voice it plays at
    // several pitches
    same("(f=x=>x*2, f(21))",               42);
    same("(f=(a,b)=>a+b, f(3,4))",          7);
    same("(f=(a,b=10)=>a+b, f(3))",         13);
    same("(f=(a,b=a*2)=>a+b, f(3))",        9);     // a default sees the earlier arg
    same("(x=5, f=x=>x*3, f(2)+x)",         11);    // the parameter does not leak
    same("(f=x=>x+1, g=y=>f(y)*2, g(4))",   10);    // nested calls

    // what must be refused rather than guessed at
    Expr e;
    check(!e.compile("[1,2]"),           "an un-indexed table is refused");
    check(!e.compile("t; t"),            "a statement is refused");
    check(!e.compile("window.foo"),      "a host object is refused");
    check(!e.compile("f=x=>{return 1}"),  "a statement body is refused");
    check(!e.compile("Array(8).fill(0)"), "a host call is refused");

    if (fails) return 1;
    std::printf("test_expr: 25 formulas agree with the hand port over 300000 ticks, "
                "and 40 coercions, draws and calls match JavaScript\n");
    return 0;
}
