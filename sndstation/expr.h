// expr.h — a bytebeat expression, compiled and played.
//
// The 25 nodes in bytebeat.h are hand-ported C++, one function each. That is
// the right way to carry 25 formulas and the wrong way to carry two thousand,
// so this is the other evaluator: it reads the JavaScript the archive is
// actually written in and plays it.
//
// JavaScript is the reference, not C, for the reasons bytebeat.h sets out at
// length -- these programs are written in C's idiom but heard through a JS
// engine, and the difference is audible. So the coercions here are JS's:
//
//   * every bitwise operator and shift runs ToInt32 on its operands, and `>>`
//     `<<` mask the shift count to 5 bits;
//   * `>>>` yields a uint32;
//   * `/` is floating-point division and `%` keeps the sign of the dividend;
//   * `&&` and `||` return an OPERAND, not a boolean, and short-circuit;
//   * an out-of-range table lookup is `undefined`, so NaN in arithmetic, and
//     ToInt32(NaN) is 0.
//
// Nothing here allocates after compile(). An expression is compiled into a
// fixed node pool and evaluated by walking it, so eval() is safe to call from
// the audio thread: no allocation, no locking, and a depth bounded at compile
// time. There are no loops in the grammar, so a program cannot fail to
// terminate.
//
// What it deliberately does NOT accept: statements (the archive's Funcbeat
// songs), assignment, variables other than `t`, and `Math.random`. The first
// two are a different language; random would make a song sound different on
// every pass, which breaks both the determinism the blend relies on and any
// hope of verifying this evaluator against a real JS engine. Anything the
// grammar cannot take is REJECTED rather than approximated -- an evaluator
// that guesses would quietly change somebody's music, which is the one
// failure this port is not allowed to have.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gritty {

// ── JavaScript's numeric coercions ──
inline int32_t jsToInt32(double d) {
    if (!std::isfinite(d)) return 0;
    double m = std::fmod(std::trunc(d), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return int32_t(uint32_t(m));
}
inline uint32_t jsToUint32(double d) { return uint32_t(jsToInt32(d)); }
inline bool     jsTruthy(double d)   { return d != 0.0 && !std::isnan(d); }

// strtod against a const pointer, which is all this parser ever has.
inline double strtodc(const char* p, const char** end) {
    char* e = nullptr;
    const double v = std::strtod(p, &e);
    *end = e;
    return v;
}

enum : uint8_t {
    OP_K, OP_T,
    OP_NEG, OP_POS, OP_BNOT, OP_LNOT,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_EXP,
    OP_SHL, OP_SHR, OP_USHR,
    OP_AND, OP_OR, OP_XOR,
    OP_LT, OP_GT, OP_LE, OP_GE, OP_EQ, OP_NE,
    OP_LAND, OP_LOR, OP_COMMA, OP_COND,
    OP_TABLE,
    OP_VAR, OP_ASSIGN, OP_RAND, OP_CALL,
    OP_F1, OP_F2, OP_FN
};
// the one-argument Math functions, indexed by Expr::Node::fi
enum : uint8_t {
    F1_SIN, F1_COS, F1_TAN, F1_ASIN, F1_ACOS, F1_ATAN, F1_FLOOR, F1_CEIL,
    F1_ROUND, F1_ABS, F1_SQRT, F1_CBRT, F1_LOG, F1_LOG2, F1_LOG10, F1_EXP,
    F1_SIGN, F1_TRUNC, F1_SINH, F1_COSH, F1_TANH, F1_ASINH, F1_ACOSH,
    F1_ATANH, F1_EXPM1, F1_LOG1P, F1_FROUND, F1_CLZ32, F1_ISNAN
};
enum : uint8_t { F2_POW, F2_ATAN2, F2_IMUL };
enum : uint8_t { FN_MIN, FN_MAX, FN_HYPOT };

// ── `random()`, made reproducible ──
//
// The archive leans on `random()` for noise -- snare tails, dust, dither --
// and refusing it costs eighty-odd songs. But true randomness would make a
// song sound different on every pass, which breaks the determinism the blend
// rests on (the same map position must sound the same every time) and makes
// verification against node impossible.
//
// So `random()` here is a hash of the sample index and the call number: white
// noise with the same distribution, drawn fresh at every tick, and identical
// on every pass and in both evaluators. The song gets the noise it asked for
// and we can still prove we are playing it right. This is a deliberate
// divergence from the composer, and the only one in the language.
// One character read as a number: its digit value in `radix`, with the two
// quirks JavaScript insists on -- a space is 0 in base 10 (`Number(" ")`), and
// anything that is not a digit of that base is NaN.
inline double charDigit(char c, int radix) {
    if (radix == 10 && c == ' ') return 0.0;
    int d = -1;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
    if (d < 0 || d >= radix) return std::nan("");
    return double(d);
}

inline double jsRandom(uint32_t t0, uint32_t& rc) {
    uint32_t h = t0 ^ (rc++ * 0x9E3779B1u);
    h = (h ^ (h >> 16)) * 0x85EBCA6Bu;
    h = (h ^ (h >> 13)) * 0xC2B2AE35u;
    h =  h ^ (h >> 16);
    return double(h) / 4294967296.0;
}

class Expr {
public:
    static constexpr int MAX_NODES = 1024;
    static constexpr int MAX_ELEM  = 2048;   // table elements, across one song
    static constexpr int MAX_ARGS  = 8;
    static constexpr int MAX_DEPTH = 160;
    // Slot 0 is `t`, which a song may assign to (`t/=4` is a common opening).
    static constexpr int MAX_SLOTS = 64;
    static constexpr int MAX_FUNCS = 16;     // arrow functions in one song
    static constexpr int MAX_PARAM = 6;
    static constexpr int MAX_CALLD = 24;     // nested calls, so recursion ends

    struct Node {
        uint8_t op = OP_K;
        uint8_t fi = 0;                 // which function, for OP_F1/F2/FN
        uint8_t n  = 0;                 // argument count, for OP_FN
        int32_t a = -1, b = -1, c = -1; // children
        int32_t ti = 0, tn = 0;         // table span, for OP_TABLE
        bool    str = false;            // ... and whether it was a string
        bool    bound = false;          // ... and whether a variable holds it
        uint8_t conv  = 0;              // radix a string element is read in; 0 = raw code
        double  k  = 0;                 // literal, for OP_K
        int32_t arg[MAX_ARGS]{};
        int32_t slot = -1;              // for OP_VAR / OP_ASSIGN
    };

    // Compile `src`. Returns true on success; on failure `error()` says why
    // and the expression must not be evaluated.
    bool compile(const char* src) {
        m_src = src; m_p = src; m_nn = 0; m_nk = 0; m_depth = 0; m_nslot = 1;
        m_nf = 0;
        for (int i = 0; i < MAX_SLOTS; ++i) { m_fnOf[i] = -1; m_tblOf[i] = -1; m_biOf[i] = -1; }
        std::memset(m_slotName, 0, sizeof m_slotName);
        std::snprintf(m_slotName[0], sizeof m_slotName[0], "t");
        m_err[0] = 0;
        const int root = parseExpr(0);
        if (root < 0) return false;
        skip();
        if (*m_p) { fail("unexpected trailing text"); return false; }
        // A table that was never indexed is an ARRAY VALUE, which this
        // evaluator has no representation for -- JavaScript would coerce it
        // through its string form. Catch it here: left unchecked it is a read
        // one slot before the node pool.
        for (int i = 0; i < m_nn; ++i)
            if (m_pool[i].op == OP_TABLE && m_pool[i].a < 0 && !m_pool[i].bound) {
                fail("a table must be indexed"); return false;
            }
        m_root = root;
        return true;
    }

    struct Frame {
        double   v[MAX_SLOTS];
        uint32_t t0 = 0;      // the sample index, for random()
        uint32_t rc = 0;      // how many random() calls so far this sample
        int      cd = 0;      // call depth, so a recursive song cannot run away
    };

    double eval(double t) const {
        // The frame lives on the caller's stack, so one compiled song can be
        // played by three lanes at once without them sharing variables.
        Frame f;
        f.v[0] = t;
        for (int i = 1; i < MAX_SLOTS; ++i) f.v[i] = std::nan("");
        f.t0 = uint32_t(jsToInt32(t));
        return ev(m_root, f);
    }

    const char* error() const { return m_err; }
    int         nodes() const { return m_nn; }

private:
    Node    m_pool[MAX_NODES];
    int32_t m_elem[MAX_ELEM];       // one node id per table element, -1 = a hole
    int     m_nn = 0, m_nk = 0, m_root = -1, m_depth = 0;
    // An arrow function. Parameters live in ordinary frame slots, saved and
    // restored around each call, which costs nothing and makes nesting --
    // and recursion, up to a limit -- behave.
    struct Func {
        int nparam = 0;
        int slot[MAX_PARAM]{};
        int dflt[MAX_PARAM]{};    // a default expression, or -1
        int body = -1;
    };
    Func   m_func[MAX_FUNCS];
    int    m_nf = 0;
    int    m_fnOf[MAX_SLOTS]{};   // which function a name holds, -1 for none
    int    m_tblOf[MAX_SLOTS]{};  // which table a name holds, -1 for none
    int    m_biOf[MAX_SLOTS]{};   // which Math function a name aliases, -1 none
    char   m_slotName[MAX_SLOTS][12]{};
    int    m_nslot = 1;                 // slot 0 is `t`
    const char* m_src = nullptr;
    const char* m_p   = nullptr;
    char   m_err[128]{};

    bool fail(const char* why) {
        if (!m_err[0]) std::snprintf(m_err, sizeof m_err, "%s at offset %d",
                                     why, int(m_p - m_src));
        return false;
    }
    int  alloc(uint8_t op) {
        if (m_nn >= MAX_NODES) { fail("expression too large"); return -1; }
        Node& nd = m_pool[m_nn]; nd = Node{}; nd.op = op;
        return m_nn++;
    }
    void skip() { while (*m_p == ' ' || *m_p == '\t' || *m_p == '\n' || *m_p == '\r') ++m_p; }
    bool eat(const char* s) {
        skip();
        const size_t n = std::strlen(s);
        if (std::strncmp(m_p, s, n) != 0) return false;
        m_p += n; return true;
    }
    char peek() { skip(); return *m_p; }

    // ── the grammar ──
    // Precedence climbing. Level 0 is the comma, 13 is unary.
    // Depth is counted where the grammar really nests -- a primary -- and
    // not once per precedence level, or a plain `a+b*c` would read as 13
    // levels deep and a normal one-liner would be refused.
    int parseExpr(int lvl) { return parseAt(lvl); }

    int parseAt(int lvl) {
        if (lvl >= 13) return parseUnary();

        // Assignment and `?:` both sit between `,` and `||`, and both are
        // right-associative. A song that opens `t/=4, a=t*3, ...` is reading
        // and writing plain local variables; they are kept in a frame that
        // starts fresh on every sample, so the expression stays a pure
        // function of `t`. A variable READ before it is written is NaN here,
        // where JavaScript would hand back whatever the previous sample left
        // in it -- so such a song renders differently from node and is
        // dropped at import rather than guessed at.
        if (lvl == 1) {
            const int cond = parseExpr(2);
            if (cond < 0) return -1;
            skip();
            if (m_pool[cond].op == OP_VAR || (m_pool[cond].op == OP_T)) {
                uint8_t cop; int clen;
                if (assignOp(cop, clen)) {
                    m_p += clen;
                    // `f = x => expr` and `f = (a, b = 2) => expr` define a
                    // function rather than compute a value. Songs use them
                    // for a voice or an envelope they then call at several
                    // pitches, which is most of what is left in the archive.
                    if (cop == 0xFF) {
                        // `C = cos, S = sin` -- a name for a built-in, which
                        // the song then calls as `C(x)`.
                        const int bi = tryBuiltinAlias();
                        if (bi >= 0) {
                            m_biOf[slotOf(cond)] = bi;
                            const int nd7 = alloc(OP_K);
                            if (nd7 < 0) return -1;
                            m_pool[nd7].k = std::nan("");
                            return nd7;
                        }
                        const int fi = tryArrow();
                        if (fi == -2) return -1;
                        if (fi >= 0) {
                            m_fnOf[slotOf(cond)] = fi;
                            const int nd2 = alloc(OP_K);
                            if (nd2 < 0) return -1;
                            m_pool[nd2].k = std::nan("");   // its numeric value
                            return nd2;
                        }
                    }
                    const int rhs = parseExpr(1);
                    if (rhs < 0) return -1;
                    // `s = [.5,.4,.3]` binds a table to a name, which the song
                    // then indexes as `s[i]` and measures as `s.length`. The
                    // binding is resolved at compile time -- there is still no
                    // array value at runtime, only a span of elements.
                    if (cop == 0xFF && m_pool[rhs].op == OP_TABLE && m_pool[rhs].a < 0) {
                        m_tblOf[slotOf(cond)] = rhs;
                        m_pool[rhs].bound = true;
                        const int nd3 = alloc(OP_K);
                        if (nd3 < 0) return -1;
                        m_pool[nd3].k = std::nan("");   // an array, used as a number
                        return nd3;
                    }
                    int val = rhs;
                    if (cop != 0xFF) {                  // a compound assignment
                        const int src2 = alloc(OP_VAR);
                        if (src2 < 0) return -1;
                        m_pool[src2].slot = slotOf(cond);
                        const int bin = alloc(cop);
                        if (bin < 0) return -1;
                        m_pool[bin].a = src2; m_pool[bin].b = rhs;
                        val = bin;
                    }
                    const int nd = alloc(OP_ASSIGN);
                    if (nd < 0) return -1;
                    m_pool[nd].slot = slotOf(cond); m_pool[nd].a = val;
                    return nd;
                }
            }
            if (*m_p != '?') return cond;
            ++m_p;
            const int a = parseExpr(1);
            if (a < 0) return -1;
            if (!eat(":")) return fail("expected ':'"), -1;
            const int b = parseExpr(1);
            if (b < 0) return -1;
            const int nd = alloc(OP_COND);
            if (nd < 0) return -1;
            m_pool[nd].a = cond; m_pool[nd].b = a; m_pool[nd].c = b;
            return nd;
        }

        int lhs = parseExpr(lvl + 1);
        if (lhs < 0) return -1;
        for (;;) {
            skip();
            uint8_t op; int len;
            if (!match(lvl, op, len)) return lhs;
            m_p += len;
            // `**` is right-associative; everything else is left.
            const int rhs = (op == OP_EXP) ? parseExpr(lvl) : parseExpr(lvl + 1);
            if (rhs < 0) return -1;
            const int nd = alloc(op);
            if (nd < 0) return -1;
            m_pool[nd].a = lhs; m_pool[nd].b = rhs;
            lhs = nd;
            if (op == OP_EXP) return lhs;
        }
    }

    // Is an arrow function next? Returns its index, -1 if not one, -2 on a
    // definite error. Needs lookahead, because `(a, b)` is a parenthesised
    // expression until the `=>` after it says otherwise.
    int tryArrow() {
        const char* save = m_p;
        skip();
        const char* start = m_p;
        char names[MAX_PARAM][12];
        int  np = 0;
        bool paren = false;
        if (*m_p == '(') {
            paren = true;
            int d = 0; const char* q = m_p;
            for (; *q; ++q) { if (*q == '(') ++d; else if (*q == ')') { if (--d == 0) { ++q; break; } } }
            const char* r = q; while (*r == ' ') ++r;
            if (!(r[0] == '=' && r[1] == '>')) { m_p = save; return -1; }
            ++m_p;
        } else if (isIdent(*m_p) && !(*m_p >= '0' && *m_p <= '9')) {
            const char* q = m_p; while (isIdent(*q)) ++q;
            const char* r = q; while (*r == ' ') ++r;
            if (!(r[0] == '=' && r[1] == '>')) { m_p = save; return -1; }
        } else { m_p = save; return -1; }
        (void)start;

        if (m_nf >= MAX_FUNCS) { fail("too many functions"); return -2; }
        Func& fn = m_func[m_nf];
        fn = Func{};
        for (int i = 0; i < MAX_PARAM; ++i) fn.dflt[i] = -1;

        // the parameter list
        for (;;) {
            skip();
            if (paren && *m_p == ')') { ++m_p; break; }
            if (!isIdent(*m_p)) { fail("bad parameter"); return -2; }
            const char* b = m_p; while (isIdent(*m_p)) ++m_p;
            const int ln = int(m_p - b);
            if (np >= MAX_PARAM) { fail("too many parameters"); return -2; }
            if (ln >= int(sizeof names[0])) { fail("parameter name too long"); return -2; }
            std::memcpy(names[np], b, size_t(ln)); names[np][ln] = 0;
            ++np;
            skip();
            if (*m_p == '=' && m_p[1] != '>' && m_p[1] != '=') {   // a default
                ++m_p;
                const int d = parseExpr(1);
                if (d < 0) return -2;
                fn.dflt[np - 1] = d;
                skip();
            }
            if (paren && *m_p == ',') { ++m_p; continue; }
            if (paren && *m_p == ')') { ++m_p; break; }
            if (!paren) break;
            fail("bad parameter list"); return -2;
        }
        skip();
        if (!(m_p[0] == '=' && m_p[1] == '>')) { fail("expected '=>'"); return -2; }
        m_p += 2;
        skip();
        if (*m_p == '{') { fail("a function body must be an expression"); return -2; }

        // Parameters get slots of their own, named so a body referring to an
        // outer variable of the same name finds the parameter, as it should.
        fn.nparam = np;
        for (int i = 0; i < np; ++i) {
            int sl = -1;
            for (int k = 0; k < m_nslot; ++k)
                if (std::strcmp(m_slotName[k], names[i]) == 0) { sl = k; break; }
            if (sl < 0) {
                if (m_nslot >= MAX_SLOTS) { fail("too many variables"); return -2; }
                sl = m_nslot++;
                std::snprintf(m_slotName[sl], sizeof m_slotName[sl], "%s", names[i]);
            }
            fn.slot[i] = sl;
        }
        const int fi = m_nf++;              // claim it before the body, for recursion
        fn.body = parseExpr(1);
        if (fn.body < 0) return -2;
        return fi;
    }

    // A bare Math name used as a value rather than called. Returns a packed
    // kind and index, or -1 if what follows is not one.
    int tryBuiltinAlias() {
        const char* save = m_p;
        skip();
        const char* b = m_p;
        if (std::strncmp(m_p, "Math.", 5) == 0) m_p += 5;
        const char* b2 = m_p;
        while (isIdent(*m_p)) ++m_p;
        const int n2 = int(m_p - b2);
        const char* after = m_p; while (*after == ' ') ++after;
        if (n2 == 0 || *after == '(') { m_p = save; (void)b; return -1; }
        const auto is2 = [&](const char* w) {
            return int(std::strlen(w)) == n2 && std::strncmp(b2, w, size_t(n2)) == 0;
        };
        struct { const char* n; int kind; int idx; } T[] = {
            {"sin", 1, F1_SIN}, {"cos", 1, F1_COS}, {"tan", 1, F1_TAN},
            {"abs", 1, F1_ABS}, {"floor", 1, F1_FLOOR}, {"ceil", 1, F1_CEIL},
            {"round", 1, F1_ROUND}, {"sqrt", 1, F1_SQRT}, {"exp", 1, F1_EXP},
            {"log", 1, F1_LOG}, {"trunc", 1, F1_TRUNC}, {"int", 1, F1_TRUNC},
            {"sign", 1, F1_SIGN}, {"tanh", 1, F1_TANH},
            {"pow", 2, F2_POW}, {"atan2", 2, F2_ATAN2},
            {"min", 3, FN_MIN}, {"max", 3, FN_MAX},
        };
        for (const auto& e : T)
            if (is2(e.n)) return e.kind * 256 + e.idx;
        m_p = save;
        return -1;
    }

    int slotOf(int node) const { return m_pool[node].op == OP_T ? 0 : m_pool[node].slot; }

    // `=` and the compound assignments. Returns 0xFF in `op` for a plain `=`,
    // otherwise the binary operator to apply first.
    bool assignOp(uint8_t& op, int& len) {
        const char* p = m_p;
        struct { const char* s; uint8_t op; } C[] = {
            {">>>=", OP_USHR}, {"<<=", OP_SHL}, {">>=", OP_SHR}, {"**=", OP_EXP},
            {"+=", OP_ADD}, {"-=", OP_SUB}, {"*=", OP_MUL}, {"/=", OP_DIV},
            {"%=", OP_MOD}, {"&=", OP_AND}, {"|=", OP_OR}, {"^=", OP_XOR},
        };
        for (const auto& c : C)
            if (std::strncmp(p, c.s, std::strlen(c.s)) == 0) {
                op = c.op; len = int(std::strlen(c.s)); return true;
            }
        if (p[0] == '=' && p[1] != '=') { op = 0xFF; len = 1; return true; }
        return false;
    }

    // Does the text at m_p start a binary operator of this level?
    bool match(int lvl, uint8_t& op, int& len) {
        const char* p = m_p;
        const char a = p[0], b = p[1], c = p[2];
        switch (lvl) {
            case 0: if (a == ',') { op = OP_COMMA; len = 1; return true; } return false;
            case 2: if (a == '|' && b == '|') { op = OP_LOR;  len = 2; return true; } return false;
            case 3: if (a == '&' && b == '&') { op = OP_LAND; len = 2; return true; } return false;
            case 4: if (a == '|' && b != '|' && b != '=') { op = OP_OR;  len = 1; return true; } return false;
            case 5: if (a == '^' && b != '=') { op = OP_XOR; len = 1; return true; } return false;
            case 6: if (a == '&' && b != '&' && b != '=') { op = OP_AND; len = 1; return true; } return false;
            case 7:
                if (a == '=' && b == '=') { op = OP_EQ; len = (c == '=') ? 3 : 2; return true; }
                if (a == '!' && b == '=') { op = OP_NE; len = (c == '=') ? 3 : 2; return true; }
                return false;
            case 8:
                if (a == '<' && b == '=') { op = OP_LE; len = 2; return true; }
                if (a == '>' && b == '=') { op = OP_GE; len = 2; return true; }
                if (a == '<' && b != '<') { op = OP_LT; len = 1; return true; }
                if (a == '>' && b != '>') { op = OP_GT; len = 1; return true; }
                return false;
            case 9:
                if (a == '>' && b == '>' && c == '>') { op = OP_USHR; len = 3; return true; }
                if (a == '>' && b == '>') { op = OP_SHR; len = 2; return true; }
                if (a == '<' && b == '<') { op = OP_SHL; len = 2; return true; }
                return false;
            case 10:
                if (a == '+' && b != '+' && b != '=') { op = OP_ADD; len = 1; return true; }
                if (a == '-' && b != '-' && b != '=') { op = OP_SUB; len = 1; return true; }
                return false;
            case 11:
                if (a == '*' && b == '*') return false;      // level 12
                if (a == '*' && b != '=') { op = OP_MUL; len = 1; return true; }
                if (a == '/' && b != '=' && b != '/' && b != '*') { op = OP_DIV; len = 1; return true; }
                if (a == '%' && b != '=') { op = OP_MOD; len = 1; return true; }
                return false;
            case 12: if (a == '*' && b == '*') { op = OP_EXP; len = 2; return true; } return false;
            default: return false;
        }
    }

    int parseUnary() {
        skip();
        uint8_t op = 0; bool is = true;
        if      (*m_p == '-') op = OP_NEG;
        else if (*m_p == '+') op = OP_POS;
        else if (*m_p == '~') op = OP_BNOT;
        else if (*m_p == '!' && m_p[1] != '=') op = OP_LNOT;
        else is = false;
        if (is) {
            ++m_p;
            const int a = parseUnary();
            if (a < 0) return -1;
            const int nd = alloc(op);
            if (nd < 0) return -1;
            m_pool[nd].a = a;
            return nd;
        }
        return parsePostfix();
    }

    int parsePostfix() {
        int lhs = parsePrimary();
        if (lhs < 0) return -1;
        for (;;) {
            skip();
            if (*m_p == '[') {
                // Indexing is only meaningful on a literal table here.
                if (m_pool[lhs].op != OP_TABLE || m_pool[lhs].a >= 0)
                    return fail("only a literal table can be indexed"), -1;
                // `"6689"[1]` is the STRING "6", and a one-character string
                // in arithmetic is its numeric VALUE, not its character code
                // -- 6, not 54. (`charCodeAt` is the other one, below.) This
                // is how a third of the archive writes a melody, so getting it
                // wrong is not a corner case; it transposes the tune.
                // The radix is recorded rather than baked into the elements,
                // so two index sites on one string stay independent and
                // parseInt can ask for base 16 over the same characters.
                if (m_pool[lhs].str) m_pool[lhs].conv = 10;
                ++m_p;
                const int idx = parseExpr(0);
                if (idx < 0) return -1;
                if (!eat("]")) return fail("expected ']'"), -1;
                m_pool[lhs].a = idx;
                continue;
            }
            if (std::strncmp(m_p, ".charCodeAt", 11) == 0) {
                if (m_pool[lhs].op != OP_TABLE || m_pool[lhs].a >= 0)
                    return fail("charCodeAt needs a string literal"), -1;
                m_p += 11;
                if (!eat("(")) return fail("expected '('"), -1;
                const int idx = parseExpr(0);
                if (idx < 0) return -1;
                if (!eat(")")) return fail("expected ')'"), -1;
                m_pool[lhs].a = idx;
                continue;
            }
            return lhs;
        }
    }

    int parsePrimary() {
        if (++m_depth > MAX_DEPTH) { --m_depth; return fail("nested too deeply"), -1; }
        const int r = primary();
        --m_depth;
        return r;
    }

    int primary() {
        skip();
        const char ch = *m_p;
        if (!ch) return fail("unexpected end of expression"), -1;

        if (ch == '(') {
            ++m_p;
            const int a = parseExpr(0);
            if (a < 0) return -1;
            if (!eat(")")) return fail("expected ')'"), -1;
            return a;
        }
        if (ch >= '0' && ch <= '9') return parseNumber();
        if (ch == '.' && m_p[1] >= '0' && m_p[1] <= '9') return parseNumber();
        if (ch == '[') return parseTable();
        if (ch == '"' || ch == '\'' || ch == '`') return parseString();
        if (ch == 't' && !isIdent(m_p[1])) { ++m_p; return alloc(OP_T); }
        // The composer compiles a song under `var {...} = Math; var int =
        // Math.trunc`, so every Math name is also a bare name, and `int` is
        // `trunc`. A song written `sin(t)` is the same song as `Math.sin(t)`.
        if (std::strncmp(m_p, "Math.", 5) == 0) { m_p += 5; return parseName(); }
        if (isIdent(ch)) return parseName();
        return fail("unexpected character"), -1;
    }

    static bool isIdent(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '$';
    }

    int parseNumber() {
        // JavaScript has binary and octal literals; strtod reads neither, and
        // stops at the `b`, leaving the digits behind as trailing text.
        if (m_p[0] == '0' && (m_p[1] == 'b' || m_p[1] == 'B' ||
                              m_p[1] == 'o' || m_p[1] == 'O')) {
            const int base = (m_p[1] == 'b' || m_p[1] == 'B') ? 2 : 8;
            const char* q = m_p + 2;
            double bv = 0; int digits = 0;
            for (; *q; ++q) {
                if (*q == '_') continue;
                const int d = (*q >= '0' && *q <= '9') ? *q - '0' : -1;
                if (d < 0 || d >= base) break;
                bv = bv * base + d; ++digits;
            }
            if (!digits) return fail("bad number"), -1;
            m_p = q;
            const int bn = alloc(OP_K);
            if (bn < 0) return -1;
            m_pool[bn].k = bv;
            return bn;
        }
        const char* end = nullptr;
        const double v = strtodc(m_p, &end);   // handles 0x.., 1e3, .5
        if (end == m_p) return fail("bad number"), -1;
        m_p = end;
        const int nd = alloc(OP_K);
        if (nd < 0) return -1;
        m_pool[nd].k = v;
        return nd;
    }

    // `[a, b, c]` — a lookup table. Elements are full expressions, not just
    // numbers: a song is as likely to write `[t>>13&5, 7-(t>>13&5), 7]` as a
    // list of constants, and only the element actually selected is evaluated.
    // A hole (`[0,2,,5]`) is `undefined`, so NaN, exactly as in JavaScript; a
    // trailing comma is not a hole and does not lengthen the table.
    int parseTable() {
        ++m_p;
        const int base = m_nk;
        bool expectElem = true;
        for (;;) {
            skip();
            if (*m_p == ']') { ++m_p; break; }
            if (*m_p == ',') {                       // an elision: a real hole
                ++m_p;
                if (m_nk >= MAX_ELEM) return fail("table too large"), -1;
                m_elem[m_nk++] = -1;
                expectElem = true;
                continue;
            }
            if (!expectElem) return fail("expected ',' or ']'"), -1;
            const int e = parseExpr(1);              // above the comma
            if (e < 0) return -1;
            if (m_nk >= MAX_ELEM) return fail("table too large"), -1;
            m_elem[m_nk++] = e;
            expectElem = false;
            skip();
            if (*m_p == ',') { ++m_p; expectElem = true; continue; }
            if (*m_p == ']') { ++m_p; break; }
            return fail("expected ',' or ']'"), -1;
        }
        const int nd = alloc(OP_TABLE);
        if (nd < 0) return -1;
        m_pool[nd].ti = base; m_pool[nd].tn = m_nk - base; m_pool[nd].a = -1;
        return nd;
    }

    // A string is a table of character codes, reachable with `.charCodeAt(i)`.
    int parseString() {
        const char q = *m_p++;
        const int base = m_nk;
        while (*m_p && *m_p != q) {
            int ch3 = uint8_t(*m_p++);
            if (ch3 == '\\') {                     // an escape
                const char e2 = *m_p++;
                switch (e2) {
                    case 'n': ch3 = '\n'; break;  case 't': ch3 = '\t'; break;
                    case 'r': ch3 = '\r'; break;  case '0': ch3 = 0;    break;
                    case 'x': case 'u': {
                        const int w = (e2 == 'x') ? 2 : 4;
                        int v2 = 0;
                        for (int k2 = 0; k2 < w; ++k2) {
                            const char h2 = *m_p;
                            const int d2 = (h2 >= '0' && h2 <= '9') ? h2 - '0'
                                         : (h2 >= 'a' && h2 <= 'f') ? h2 - 'a' + 10
                                         : (h2 >= 'A' && h2 <= 'F') ? h2 - 'A' + 10 : -1;
                            if (d2 < 0) return fail("bad escape"), -1;
                            v2 = v2 * 16 + d2; ++m_p;
                        }
                        ch3 = v2; break;
                    }
                    case 0: return fail("unterminated string"), -1;
                    default: ch3 = uint8_t(e2); break;   // \\ \' \" and the rest
                }
            }
            if (m_nk >= MAX_ELEM) return fail("string too long"), -1;
            const int kn = alloc(OP_K);
            if (kn < 0) return -1;
            m_pool[kn].k = double(ch3);
            m_elem[m_nk++] = kn;
        }
        if (*m_p != q) return fail("unterminated string"), -1;
        ++m_p;
        const int nd = alloc(OP_TABLE);
        if (nd < 0) return -1;
        m_pool[nd].ti = base; m_pool[nd].tn = m_nk - base; m_pool[nd].a = -1;
        m_pool[nd].str = true;
        return nd;
    }

    int parseName() {
        const char* s = m_p;
        while (isIdent(*m_p)) ++m_p;
        const int n = int(m_p - s);
        const auto is = [&](const char* w) {
            return int(std::strlen(w)) == n && std::strncmp(s, w, size_t(n)) == 0;
        };
        // the constants
        double kv = 0; bool isK = true;
        if      (is("PI"))     kv = 3.141592653589793;
        else if (is("E"))      kv = 2.718281828459045;
        else if (is("LN2"))    kv = 0.6931471805599453;
        else if (is("LN10"))   kv = 2.302585092994046;
        else if (is("LOG2E"))  kv = 1.4426950408889634;
        else if (is("LOG10E")) kv = 0.4342944819032518;
        else if (is("SQRT2"))  kv = 1.4142135623730951;
        else isK = false;
        if (isK) {
            const int nd = alloc(OP_K);
            if (nd < 0) return -1;
            m_pool[nd].k = kv;
            return nd;
        }
        // `parseInt(s[i], 16)` is how a song packs a hex pattern into a
        // string, and `parseInt(x)` on a number is its truncation. The first
        // is folded into the index site's radix; the second is `trunc`.
        if (is("parseInt")) {
            if (!eat("(")) return fail("expected '('"), -1;
            const int a4 = parseExpr(1);
            if (a4 < 0) return -1;
            int radix = 10;
            if (eat(",")) {
                const int r4 = parseExpr(1);
                if (r4 < 0) return -1;
                if (m_pool[r4].op != OP_K) return fail("parseInt needs a constant radix"), -1;
                radix = int(m_pool[r4].k);
                if (radix < 2 || radix > 36) return fail("bad radix"), -1;
            }
            if (!eat(")")) return fail("expected ')'"), -1;
            if (m_pool[a4].op == OP_TABLE && m_pool[a4].str && m_pool[a4].a >= 0) {
                m_pool[a4].conv = uint8_t(radix);
                return a4;
            }
            if (radix != 10) return fail("parseInt needs a string in that radix"), -1;
            const int nd6 = alloc(OP_F1);
            if (nd6 < 0) return -1;
            m_pool[nd6].fi = F1_TRUNC; m_pool[nd6].a = a4;
            return nd6;
        }
        if (is("random")) {
            if (!eat("(")) return fail("expected '('"), -1;
            if (!eat(")")) return fail("random takes no arguments"), -1;
            return alloc(OP_RAND);
        }

        uint8_t f1 = 0; bool has1 = true;
        if      (is("sin"))   f1 = F1_SIN;   else if (is("cos"))   f1 = F1_COS;
        else if (is("tan"))   f1 = F1_TAN;   else if (is("asin"))  f1 = F1_ASIN;
        else if (is("acos"))  f1 = F1_ACOS;  else if (is("atan"))  f1 = F1_ATAN;
        else if (is("floor")) f1 = F1_FLOOR; else if (is("ceil"))  f1 = F1_CEIL;
        else if (is("round")) f1 = F1_ROUND; else if (is("abs"))   f1 = F1_ABS;
        else if (is("sqrt"))  f1 = F1_SQRT;  else if (is("cbrt"))  f1 = F1_CBRT;
        else if (is("log"))   f1 = F1_LOG;   else if (is("log2"))  f1 = F1_LOG2;
        else if (is("log10")) f1 = F1_LOG10; else if (is("exp"))   f1 = F1_EXP;
        else if (is("sign"))  f1 = F1_SIGN;  else if (is("trunc")) f1 = F1_TRUNC;
        else if (is("int"))   f1 = F1_TRUNC;      // the header's `var int = Math.trunc`
        else if (is("sinh"))  f1 = F1_SINH;   else if (is("cosh"))  f1 = F1_COSH;
        else if (is("tanh"))  f1 = F1_TANH;   else if (is("asinh")) f1 = F1_ASINH;
        else if (is("acosh")) f1 = F1_ACOSH;  else if (is("atanh")) f1 = F1_ATANH;
        else if (is("expm1")) f1 = F1_EXPM1;  else if (is("log1p")) f1 = F1_LOG1P;
        else if (is("fround"))f1 = F1_FROUND; else if (is("clz32")) f1 = F1_CLZ32;
        else if (is("isNaN")) f1 = F1_ISNAN;
        else has1 = false;

        uint8_t f2 = 0; bool has2 = true;
        if      (is("pow"))   f2 = F2_POW;
        else if (is("atan2")) f2 = F2_ATAN2;
        else if (is("imul"))  f2 = F2_IMUL;
        else has2 = false;

        uint8_t fn = 0; bool hasN = true;
        if      (is("min"))   fn = FN_MIN;
        else if (is("max"))   fn = FN_MAX;
        else if (is("hypot")) fn = FN_HYPOT;
        else hasN = false;

        if (!has1 && !has2 && !hasN) {
            // Not a Math name, so a variable. A host object (`Array`,
            // `parseInt`, `this`, `function`) is told apart not by the length
            // of its name but by what follows it: a call or a property
            // access. A plain name standing on its own is a variable, and
            // songs name theirs `sine` and `tension` as readily as `a`.
            if (n >= int(sizeof m_slotName[0])) return fail("name too long"), -1;
            skip();
            const bool prop = (*m_p == '.');
            char nm[12];
            const int ln = n < 11 ? n : 11;
            std::memcpy(nm, s, size_t(ln)); nm[ln] = 0;
            int sl = -1;
            for (int i = 0; i < m_nslot; ++i)
                if (std::strcmp(m_slotName[i], nm) == 0) { sl = i; break; }
            if (sl < 0) {
                if (m_nslot >= MAX_SLOTS) return fail("too many variables"), -1;
                sl = m_nslot++;
                std::snprintf(m_slotName[sl], sizeof m_slotName[sl], "%s", nm);
            }
            if (prop) {
                if (m_tblOf[sl] < 0) return fail("unsupported property"), -1;
                if (std::strncmp(m_p, ".length", 7) == 0) {
                    m_p += 7;
                    const int nd4 = alloc(OP_K);
                    if (nd4 < 0) return -1;
                    m_pool[nd4].k = double(m_pool[m_tblOf[sl]].tn);
                    return nd4;
                }
                // `s.charCodeAt(i)` where a variable holds the string: hand
                // back a table over the same elements and let parsePostfix
                // attach the index, exactly as for a literal.
                if (std::strncmp(m_p, ".charCodeAt", 11) == 0) {
                    const Node& src3 = m_pool[m_tblOf[sl]];
                    const int nd9 = alloc(OP_TABLE);
                    if (nd9 < 0) return -1;
                    m_pool[nd9].ti = src3.ti; m_pool[nd9].tn = src3.tn;
                    m_pool[nd9].str = src3.str; m_pool[nd9].a = -1;
                    return nd9;
                }
                return fail("unsupported property"), -1;
            }
            skip();
            if (*m_p == '[' && m_tblOf[sl] >= 0) {
                // A fresh table node over the same elements, so parsePostfix
                // attaches the index as it does to a literal. It must be a
                // COPY: indexing a string rewrites its elements from character
                // codes to numeric values, and two index sites on one string
                // must not rewrite each other's.
                const Node& src2 = m_pool[m_tblOf[sl]];
                const int nd5 = alloc(OP_TABLE);
                if (nd5 < 0) return -1;
                m_pool[nd5].ti = src2.ti; m_pool[nd5].tn = src2.tn;
                m_pool[nd5].str = src2.str;
                m_pool[nd5].a = -1;
                return nd5;
            }
            if (*m_p == '(' && m_fnOf[sl] < 0 && m_biOf[sl] >= 0) {
                const int kind = m_biOf[sl] / 256, idx = m_biOf[sl] % 256;
                ++m_p;
                int a5[MAX_ARGS]; int na5 = 0;
                if (peek() != ')') {
                    for (;;) {
                        if (na5 >= MAX_ARGS) return fail("too many arguments"), -1;
                        const int e5 = parseExpr(1);
                        if (e5 < 0) return -1;
                        a5[na5++] = e5;
                        if (eat(",")) continue;
                        break;
                    }
                }
                if (!eat(")")) return fail("expected ')'"), -1;
                if (kind == 1) {
                    if (na5 != 1) return fail("expected one argument"), -1;
                    const int nd8 = alloc(OP_F1);
                    if (nd8 < 0) return -1;
                    m_pool[nd8].fi = uint8_t(idx); m_pool[nd8].a = a5[0];
                    return nd8;
                }
                if (kind == 2) {
                    if (na5 != 2) return fail("expected two arguments"), -1;
                    const int nd8 = alloc(OP_F2);
                    if (nd8 < 0) return -1;
                    m_pool[nd8].fi = uint8_t(idx); m_pool[nd8].a = a5[0]; m_pool[nd8].b = a5[1];
                    return nd8;
                }
                const int nd8 = alloc(OP_FN);
                if (nd8 < 0) return -1;
                m_pool[nd8].fi = uint8_t(idx); m_pool[nd8].n = uint8_t(na5);
                for (int i = 0; i < na5; ++i) m_pool[nd8].arg[i] = a5[i];
                return nd8;
            }
            if (*m_p == '(') {
                if (m_fnOf[sl] < 0) return fail("unsupported call"), -1;
                ++m_p;
                int a2[MAX_ARGS]; int na2 = 0;
                if (peek() != ')') {
                    for (;;) {
                        if (na2 >= MAX_ARGS) return fail("too many arguments"), -1;
                        const int a3 = parseExpr(1);
                        if (a3 < 0) return -1;
                        a2[na2++] = a3;
                        if (eat(",")) continue;
                        break;
                    }
                }
                if (!eat(")")) return fail("expected ')'"), -1;
                const int nd2 = alloc(OP_CALL);
                if (nd2 < 0) return -1;
                m_pool[nd2].slot = m_fnOf[sl];
                m_pool[nd2].n = uint8_t(na2);
                for (int i = 0; i < na2; ++i) m_pool[nd2].arg[i] = a2[i];
                return nd2;
            }
            const int nd = alloc(OP_VAR);
            if (nd < 0) return -1;
            m_pool[nd].slot = sl;
            return nd;
        }
        if (!eat("(")) return fail("expected '('"), -1;

        int arg[MAX_ARGS]; int na = 0;
        if (peek() != ')') {
            for (;;) {
                if (na >= MAX_ARGS) return fail("too many arguments"), -1;
                const int a = parseExpr(1);     // above the comma: it separates here
                if (a < 0) return -1;
                arg[na++] = a;
                if (eat(",")) continue;
                break;
            }
        }
        if (!eat(")")) return fail("expected ')'"), -1;

        if (has1) {
            if (na != 1) return fail("expected one argument"), -1;
            const int nd = alloc(OP_F1);
            if (nd < 0) return -1;
            m_pool[nd].fi = f1; m_pool[nd].a = arg[0];
            return nd;
        }
        if (has2) {
            if (na != 2) return fail("expected two arguments"), -1;
            const int nd = alloc(OP_F2);
            if (nd < 0) return -1;
            m_pool[nd].fi = f2; m_pool[nd].a = arg[0]; m_pool[nd].b = arg[1];
            return nd;
        }
        const int nd = alloc(OP_FN);
        if (nd < 0) return -1;
        m_pool[nd].fi = fn; m_pool[nd].n = uint8_t(na);
        for (int i = 0; i < na; ++i) m_pool[nd].arg[i] = arg[i];
        return nd;
    }

    // ── evaluation ──
    double ev(int id, Frame& f) const {
        if (id < 0 || id >= m_nn) return std::nan("");   // belt and braces
        const Node& n = m_pool[id];
        switch (n.op) {
            case OP_K:    return n.k;
            case OP_VAR:  return f.v[n.slot];
            case OP_ASSIGN: return f.v[n.slot] = ev(n.a, f);
            case OP_RAND: return jsRandom(f.t0, f.rc);
            case OP_CALL: {
                const Func& fn = m_func[n.slot];
                if (f.cd >= MAX_CALLD) return std::nan("");   // runaway recursion
                // Arguments are evaluated BEFORE the parameter slots are
                // touched, since an argument may read the slot it is about to
                // fill (`v(t, p)` inside `v` itself).
                double in[MAX_PARAM];
                for (int i = 0; i < fn.nparam; ++i)
                    in[i] = i < n.n ? ev(n.arg[i], f) : std::nan("");
                double saved[MAX_PARAM];
                for (int i = 0; i < fn.nparam; ++i) saved[i] = f.v[fn.slot[i]];
                for (int i = 0; i < fn.nparam; ++i) {
                    // A default is evaluated with the earlier parameters
                    // already bound, which is the order JavaScript uses.
                    f.v[fn.slot[i]] = (i < n.n) ? in[i]
                                     : (fn.dflt[i] >= 0 ? ev(fn.dflt[i], f) : std::nan(""));
                }
                ++f.cd;
                const double r = ev(fn.body, f);
                --f.cd;
                for (int i = 0; i < fn.nparam; ++i) f.v[fn.slot[i]] = saved[i];
                return r;
            }
            case OP_T:    return f.v[0];
            case OP_NEG:  return -ev(n.a, f);
            case OP_POS:  return  ev(n.a, f);
            case OP_BNOT: return double(~jsToInt32(ev(n.a, f)));
            case OP_LNOT: return jsTruthy(ev(n.a, f)) ? 0.0 : 1.0;

            case OP_ADD:  return ev(n.a, f) + ev(n.b, f);
            case OP_SUB:  return ev(n.a, f) - ev(n.b, f);
            case OP_MUL:  return ev(n.a, f) * ev(n.b, f);
            case OP_DIV:  return ev(n.a, f) / ev(n.b, f);
            case OP_MOD:  return std::fmod(ev(n.a, f), ev(n.b, f));
            case OP_EXP:  return std::pow(ev(n.a, f), ev(n.b, f));

            case OP_SHL:  return double(int32_t(jsToUint32(ev(n.a, f)) << (jsToUint32(ev(n.b, f)) & 31u)));
            case OP_SHR:  return double(jsToInt32(ev(n.a, f)) >> (jsToUint32(ev(n.b, f)) & 31u));
            case OP_USHR: return double(jsToUint32(ev(n.a, f)) >> (jsToUint32(ev(n.b, f)) & 31u));

            case OP_AND:  return double(jsToInt32(ev(n.a, f)) & jsToInt32(ev(n.b, f)));
            case OP_OR:   return double(jsToInt32(ev(n.a, f)) | jsToInt32(ev(n.b, f)));
            case OP_XOR:  return double(jsToInt32(ev(n.a, f)) ^ jsToInt32(ev(n.b, f)));

            case OP_LT:   return ev(n.a, f) <  ev(n.b, f) ? 1.0 : 0.0;
            case OP_GT:   return ev(n.a, f) >  ev(n.b, f) ? 1.0 : 0.0;
            case OP_LE:   return ev(n.a, f) <= ev(n.b, f) ? 1.0 : 0.0;
            case OP_GE:   return ev(n.a, f) >= ev(n.b, f) ? 1.0 : 0.0;
            case OP_EQ:   return ev(n.a, f) == ev(n.b, f) ? 1.0 : 0.0;
            case OP_NE:   return ev(n.a, f) != ev(n.b, f) ? 1.0 : 0.0;

            // These return an operand, as JS does, and short-circuit.
            case OP_LAND: { const double a = ev(n.a, f); return jsTruthy(a) ? ev(n.b, f) : a; }
            case OP_LOR:  { const double a = ev(n.a, f); return jsTruthy(a) ? a : ev(n.b, f); }
            case OP_COMMA: ev(n.a, f); return ev(n.b, f);
            case OP_COND: return jsTruthy(ev(n.a, f)) ? ev(n.b, f) : ev(n.c, f);

            case OP_TABLE: {
                const double fi = ev(n.a, f);
                if (!(fi >= 0) || fi != std::floor(fi) || fi >= double(n.tn))
                    return std::nan("");          // `undefined` in arithmetic
                const int32_t e = m_elem[n.ti + int(fi)];
                if (e < 0) return std::nan("");           // a hole is undefined
                const double raw = ev(e, f);
                if (!n.conv) return raw;
                return charDigit(char(int(raw)), n.conv);
            }
            case OP_F1: {
                const double a = ev(n.a, f);
                switch (n.fi) {
                    case F1_SIN:   return std::sin(a);   case F1_COS:  return std::cos(a);
                    case F1_TAN:   return std::tan(a);   case F1_ASIN: return std::asin(a);
                    case F1_ACOS:  return std::acos(a);  case F1_ATAN: return std::atan(a);
                    case F1_FLOOR: return std::floor(a); case F1_CEIL: return std::ceil(a);
                    case F1_ROUND: return std::floor(a + 0.5);   // JS rounds halves up
                    case F1_ABS:   return std::fabs(a);  case F1_SQRT: return std::sqrt(a);
                    case F1_CBRT:  return std::cbrt(a);  case F1_LOG:  return std::log(a);
                    case F1_LOG2:  return std::log2(a);  case F1_LOG10:return std::log10(a);
                    case F1_EXP:   return std::exp(a);
                    case F1_SIGN:  return std::isnan(a) ? a : (a > 0 ? 1.0 : (a < 0 ? -1.0 : a));
                    case F1_SINH:  return std::sinh(a);  case F1_COSH:  return std::cosh(a);
                    case F1_TANH:  return std::tanh(a);  case F1_ASINH: return std::asinh(a);
                    case F1_ACOSH: return std::acosh(a); case F1_ATANH: return std::atanh(a);
                    case F1_EXPM1: return std::expm1(a); case F1_LOG1P: return std::log1p(a);
                    case F1_FROUND:return double(float(a));
                    case F1_CLZ32: {
                        const uint32_t u = jsToUint32(a);
                        if (!u) return 32;
                        int c = 0; for (uint32_t m = 0x80000000u; !(u & m); m >>= 1) ++c;
                        return double(c);
                    }
                    case F1_ISNAN: return std::isnan(a) ? 1.0 : 0.0;
                    default:       return std::trunc(a);
                }
            }
            case OP_F2: {
                const double a = ev(n.a, f), b = ev(n.b, f);
                if (n.fi == F2_POW)   return std::pow(a, b);
                if (n.fi == F2_ATAN2) return std::atan2(a, b);
                return double(int32_t(jsToUint32(a) * jsToUint32(b)));   // Math.imul
            }
            default: {
                if (n.fi == FN_HYPOT) {
                    double s = 0;
                    for (int i = 0; i < n.n; ++i) { const double v = ev(n.arg[i], f); s += v * v; }
                    return std::sqrt(s);
                }
                // Math.min() is Infinity and Math.max() is -Infinity, and a
                // NaN argument poisons both.
                double r = (n.fi == FN_MIN) ? HUGE_VAL : -HUGE_VAL;
                for (int i = 0; i < n.n; ++i) {
                    const double v = ev(n.arg[i], f);
                    if (std::isnan(v)) return v;
                    if (n.fi == FN_MIN ? (v < r) : (v > r)) r = v;
                }
                return r;
            }
        }
    }
};

// ── a song's output, as a byte ──
// The engine is a bytebeat engine: a node is a byte at a tick. The archive's
// four playback modes differ only in how a number becomes that byte.
enum : uint8_t { MODE_BYTE = 0, MODE_SIGNED = 1, MODE_FLOAT = 2 };

// In double, which is what the language is defined in. The verifier compares
// this against node, so a disagreement means a difference in SEMANTICS and not
// merely in storage -- float32 would blur the two, and a song that is really
// being played right would look like one that is not.
inline double exprValue(const Expr& e, int mode, double t) {
    const double v = e.eval(t);
    if (mode == MODE_FLOAT) {
        if (std::isnan(v)) return 127.5;
        const double c = v < -1 ? -1 : (v > 1 ? 1 : v);
        return (c + 1.0) * 127.5;                 // kept fractional, not rounded
    }
    const int32_t i = jsToInt32(v);
    return double(uint32_t(mode == MODE_SIGNED ? i + 128 : i) & 255u);
}

// What the engine plays. Single precision is ordinary for audio and still far
// finer than the eight bits a bytebeat byte carries.
inline float exprByte(const Expr& e, int mode, double t) {
    return float(exprValue(e, mode, t));
}

} // namespace gritty
