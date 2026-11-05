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
    OP_VAR, OP_ASSIGN,
    OP_F1, OP_F2, OP_FN
};
// the one-argument Math functions, indexed by Expr::Node::fi
enum : uint8_t {
    F1_SIN, F1_COS, F1_TAN, F1_ASIN, F1_ACOS, F1_ATAN, F1_FLOOR, F1_CEIL,
    F1_ROUND, F1_ABS, F1_SQRT, F1_CBRT, F1_LOG, F1_LOG2, F1_LOG10, F1_EXP,
    F1_SIGN, F1_TRUNC
};
enum : uint8_t { F2_POW, F2_ATAN2 };
enum : uint8_t { FN_MIN, FN_MAX, FN_HYPOT };

class Expr {
public:
    static constexpr int MAX_NODES = 1024;
    static constexpr int MAX_CONST = 1024;
    static constexpr int MAX_ARGS  = 8;
    static constexpr int MAX_DEPTH = 160;
    // Slot 0 is `t`, which a song may assign to (`t/=4` is a common opening).
    static constexpr int MAX_SLOTS = 48;

    struct Node {
        uint8_t op = OP_K;
        uint8_t fi = 0;                 // which function, for OP_F1/F2/FN
        uint8_t n  = 0;                 // argument count, for OP_FN
        int32_t a = -1, b = -1, c = -1; // children
        int32_t ti = 0, tn = 0;         // table span, for OP_TABLE
        bool    str = false;            // ... and whether it was a string
        double  k  = 0;                 // literal, for OP_K
        int32_t arg[MAX_ARGS]{};
        int32_t slot = -1;              // for OP_VAR / OP_ASSIGN
    };

    // Compile `src`. Returns true on success; on failure `error()` says why
    // and the expression must not be evaluated.
    bool compile(const char* src) {
        m_src = src; m_p = src; m_nn = 0; m_nk = 0; m_depth = 0; m_nslot = 1;
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
            if (m_pool[i].op == OP_TABLE && m_pool[i].a < 0) {
                fail("a table must be indexed"); return false;
            }
        m_root = root;
        return true;
    }

    double eval(double t) const {
        // The frame lives on the caller's stack, so one compiled song can be
        // played by three lanes at once without them sharing variables.
        double slot[MAX_SLOTS];
        slot[0] = t;
        for (int i = 1; i < MAX_SLOTS; ++i) slot[i] = std::nan("");
        return ev(m_root, slot);
    }

    const char* error() const { return m_err; }
    int         nodes() const { return m_nn; }

private:
    Node   m_pool[MAX_NODES];
    double m_kpool[MAX_CONST];
    int    m_nn = 0, m_nk = 0, m_root = -1, m_depth = 0;
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
                    const int rhs = parseExpr(1);
                    if (rhs < 0) return -1;
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
                // is how a third of the archive writes a melody, so getting
                // it wrong is not a corner case; it transposes the tune.
                if (m_pool[lhs].str)
                    for (int j = 0; j < m_pool[lhs].tn; ++j) {
                        const char ch2 = char(int(m_kpool[m_pool[lhs].ti + j]));
                        m_kpool[m_pool[lhs].ti + j] =
                            (ch2 >= '0' && ch2 <= '9') ? double(ch2 - '0')
                          : (ch2 == ' ') ? 0.0           // Number(" ") is 0
                          : std::nan("");                // Number("a") is NaN
                    }
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
        const char* end = nullptr;
        const double v = strtodc(m_p, &end);   // handles 0x.., 1e3, .5
        if (end == m_p) return fail("bad number"), -1;
        m_p = end;
        const int nd = alloc(OP_K);
        if (nd < 0) return -1;
        m_pool[nd].k = v;
        return nd;
    }

    // `[a,b,c]` — a constant lookup table. Non-constant elements are rejected
    // rather than evaluated, because a table is read once at compile time.
    int parseTable() {
        ++m_p;
        const int base = m_nk;
        for (;;) {
            skip();
            if (*m_p == ']') { ++m_p; break; }
            const char* end = nullptr;
            skip();
            bool neg = false;
            if (*m_p == '-') { neg = true; ++m_p; skip(); }
            const double v = strtodc(m_p, &end);
            if (end == m_p) return fail("a table may hold only numbers"), -1;
            m_p = end;
            if (m_nk >= MAX_CONST) return fail("table too large"), -1;
            m_kpool[m_nk++] = neg ? -v : v;
            skip();
            if (*m_p == ',') { ++m_p; continue; }
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
            if (*m_p == '\\') return fail("escapes in strings are not supported"), -1;
            if (m_nk >= MAX_CONST) return fail("string too long"), -1;
            m_kpool[m_nk++] = double(uint8_t(*m_p++));
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
        if (is("random")) return fail("random is not supported"), -1;

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
        else has1 = false;

        uint8_t f2 = 0; bool has2 = true;
        if      (is("pow"))   f2 = F2_POW;
        else if (is("atan2")) f2 = F2_ATAN2;
        else has2 = false;

        uint8_t fn = 0; bool hasN = true;
        if      (is("min"))   fn = FN_MIN;
        else if (is("max"))   fn = FN_MAX;
        else if (is("hypot")) fn = FN_HYPOT;
        else hasN = false;

        if (!has1 && !has2 && !hasN) {
            // Not a Math name, so a variable. Short names only: anything
            // longer is a host object this evaluator has no business guessing
            // at (`this`, `window`, `Array` and friends).
            if (n > 3) return fail("unsupported name"), -1;
            skip();
            if (*m_p == '(' || *m_p == '.') return fail("unsupported call"), -1;
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
    double ev(int id, double* sl) const {
        if (id < 0 || id >= m_nn) return std::nan("");   // belt and braces
        const Node& n = m_pool[id];
        switch (n.op) {
            case OP_K:    return n.k;
            case OP_VAR:  return sl[n.slot];
            case OP_ASSIGN: return sl[n.slot] = ev(n.a, sl);
            case OP_T:    return sl[0];
            case OP_NEG:  return -ev(n.a, sl);
            case OP_POS:  return  ev(n.a, sl);
            case OP_BNOT: return double(~jsToInt32(ev(n.a, sl)));
            case OP_LNOT: return jsTruthy(ev(n.a, sl)) ? 0.0 : 1.0;

            case OP_ADD:  return ev(n.a, sl) + ev(n.b, sl);
            case OP_SUB:  return ev(n.a, sl) - ev(n.b, sl);
            case OP_MUL:  return ev(n.a, sl) * ev(n.b, sl);
            case OP_DIV:  return ev(n.a, sl) / ev(n.b, sl);
            case OP_MOD:  return std::fmod(ev(n.a, sl), ev(n.b, sl));
            case OP_EXP:  return std::pow(ev(n.a, sl), ev(n.b, sl));

            case OP_SHL:  return double(int32_t(jsToUint32(ev(n.a, sl)) << (jsToUint32(ev(n.b, sl)) & 31u)));
            case OP_SHR:  return double(jsToInt32(ev(n.a, sl)) >> (jsToUint32(ev(n.b, sl)) & 31u));
            case OP_USHR: return double(jsToUint32(ev(n.a, sl)) >> (jsToUint32(ev(n.b, sl)) & 31u));

            case OP_AND:  return double(jsToInt32(ev(n.a, sl)) & jsToInt32(ev(n.b, sl)));
            case OP_OR:   return double(jsToInt32(ev(n.a, sl)) | jsToInt32(ev(n.b, sl)));
            case OP_XOR:  return double(jsToInt32(ev(n.a, sl)) ^ jsToInt32(ev(n.b, sl)));

            case OP_LT:   return ev(n.a, sl) <  ev(n.b, sl) ? 1.0 : 0.0;
            case OP_GT:   return ev(n.a, sl) >  ev(n.b, sl) ? 1.0 : 0.0;
            case OP_LE:   return ev(n.a, sl) <= ev(n.b, sl) ? 1.0 : 0.0;
            case OP_GE:   return ev(n.a, sl) >= ev(n.b, sl) ? 1.0 : 0.0;
            case OP_EQ:   return ev(n.a, sl) == ev(n.b, sl) ? 1.0 : 0.0;
            case OP_NE:   return ev(n.a, sl) != ev(n.b, sl) ? 1.0 : 0.0;

            // These return an operand, as JS does, and short-circuit.
            case OP_LAND: { const double a = ev(n.a, sl); return jsTruthy(a) ? ev(n.b, sl) : a; }
            case OP_LOR:  { const double a = ev(n.a, sl); return jsTruthy(a) ? a : ev(n.b, sl); }
            case OP_COMMA: ev(n.a, sl); return ev(n.b, sl);
            case OP_COND: return jsTruthy(ev(n.a, sl)) ? ev(n.b, sl) : ev(n.c, sl);

            case OP_TABLE: {
                const double fi = ev(n.a, sl);
                if (!(fi >= 0) || fi != std::floor(fi) || fi >= double(n.tn))
                    return std::nan("");          // `undefined` in arithmetic
                return m_kpool[n.ti + int(fi)];
            }
            case OP_F1: {
                const double a = ev(n.a, sl);
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
                    default:       return std::trunc(a);
                }
            }
            case OP_F2: {
                const double a = ev(n.a, sl), b = ev(n.b, sl);
                return n.fi == F2_POW ? std::pow(a, b) : std::atan2(a, b);
            }
            default: {
                if (n.fi == FN_HYPOT) {
                    double s = 0;
                    for (int i = 0; i < n.n; ++i) { const double v = ev(n.arg[i], sl); s += v * v; }
                    return std::sqrt(s);
                }
                // Math.min() is Infinity and Math.max() is -Infinity, and a
                // NaN argument poisons both.
                double r = (n.fi == FN_MIN) ? HUGE_VAL : -HUGE_VAL;
                for (int i = 0; i < n.n; ++i) {
                    const double v = ev(n.arg[i], sl);
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

inline float exprByte(const Expr& e, int mode, double t) {
    const double v = e.eval(t);
    if (mode == MODE_FLOAT) {
        if (std::isnan(v)) return 127.5f;
        const double c = v < -1 ? -1 : (v > 1 ? 1 : v);
        return float((c + 1.0) * 127.5);          // kept fractional, not rounded
    }
    const int32_t i = jsToInt32(v);
    return float(uint32_t(mode == MODE_SIGNED ? i + 128 : i) & 255u);
}

} // namespace gritty
