// bytebeat.h — the 25 map nodes, as C++, with JavaScript's integer semantics.
//
// The web demo (../demos/gritty-grids.html) and the Max port
// (../max/gritty-grids.genexpr) both evaluate these one-liners; this is the
// third evaluator, and the only one that has to say out loud what the other
// two got from their host language.
//
// Classic bytebeat is written in C's int arithmetic but almost always HEARD
// through a JavaScript engine, so JS is the reference here, not C:
//
//   * `a >> b` and `a << b` coerce `a` with ToInt32 (wrap to 32 bits, signed)
//     and mask `b` to its low 5 bits. A shift by 33 is a shift by 1, and a
//     product that has overflowed 32 bits is a NEGATIVE operand by the time
//     it reaches the shift. Node 1 is audibly this and nothing else.
//   * `a * b` is a DOUBLE multiply. The wrap happens later, at whatever
//     bitwise op consumes it (here always `& 255`), and the low 8 bits of an
//     exact integer product are the same whether you wrap at 32 bits or not.
//     So the products below stay in int64 and only the shifts wrap.
//   * `0x8898a989` is a positive JS number, but `>>` makes it the NEGATIVE
//     int32 -2003400311 and shifts it arithmetically. Node 24 is that.
//
// The one place this port is deliberately not bit-exact: JS loses the low
// bits of a product past 2^53, which the Bit voice engine's free-running `t`
// reaches after a few hours of continuous play. int64 keeps them. The port
// is the more correct of the two; nobody will hear the difference.
#pragma once
#include <cstdint>

namespace gritty {

// ToInt32: the coercion every JS bitwise operator performs on its operands.
inline int32_t i32(int64_t v) { return int32_t(uint32_t(uint64_t(v))); }

// `a >> b` and `a << b` as JS performs them.
inline int64_t SR(int64_t a, int64_t b) {
    return int64_t(i32(a) >> int(uint32_t(i32(b)) & 31u));
}
inline int64_t SL(int64_t a, int64_t b) {
    return int64_t(int32_t(uint32_t(i32(a)) << int(uint32_t(i32(b)) & 31u)));
}

// The 25 nodes of the 5x5 map: 25 real one-liners from viznut's "Collection
// of oneliner music formulas" (2011-10-18), exact and attributed, in the same
// order as the web demo's NODES — so a map position means the same thing in
// all three ports.
using NodeFn = int64_t (*)(int64_t t);

inline int64_t n00(int64_t t) { return t * ((SR(t,12) | SR(t,8)) & (63 & SR(t,4))); }
inline int64_t n01(int64_t t) { return SR(t * (SR(t,5) | SR(t,8)), SR(t,16)); }
inline int64_t n02(int64_t t) { return t * ((SR(t,9) | SR(t,13)) & (25 & SR(t,6))); }
inline int64_t n03(int64_t t) { return t * ((SR(t,11) & SR(t,8)) & (123 & SR(t,3))); }
inline int64_t n04(int64_t t) { return t * (SR(t, SR(t,9) | SR(t,8)) & (63 & SR(t,4))); }
inline int64_t n05(int64_t t) { return t * (42 & SR(t,10)); }
inline int64_t n06(int64_t t) { return i32(t) & SR(t,8); }
inline int64_t n07(int64_t t) { return (SR(t,6) | i32(t) | SR(t, SR(t,16))) * 10 + (SR(t,11) & 7); }
inline int64_t n08(int64_t t) { return (t | SR(t,9) | SR(t,7)) * t & (SR(t,11) | SR(t,9)); }
inline int64_t n09(int64_t t) { return (t * 5 & SR(t,7)) | (t * 3 & SR(t * 4, 10)); }
inline int64_t n10(int64_t t) { return (SR(t,7) | i32(t) | SR(t,6)) * 10 + 4 * ((i32(t) & SR(t,13)) | SR(t,6)); }
inline int64_t n11(int64_t t) { return ((t * (SR(t,8) | SR(t,9)) & 46 & SR(t,8))) ^ ((i32(t) & SR(t,13)) | SR(t,6)); }
inline int64_t n12(int64_t t) { return (t * 5 & SR(t,7)) | (t * 3 & SR(t,10)); }
inline int64_t n13(int64_t t) { return ((t * 9 & SR(t,4)) | (t * 5 & SR(t,7)) | (t * 3 & int64_t(double(t) / 1024.0))) - 1; }
inline int64_t n14(int64_t t) { return (t * (SR(t,9) | SR(t,13))) & 16; }
inline int64_t n15(int64_t t) { return (i32(t) & SR(t,12)) * (SR(t,4) | SR(t,8)); }
inline int64_t n16(int64_t t) { return (i32(t) & (i32(t) % 255)) - (t * 3 & SR(t,13) & SR(t,6)); }
inline int64_t n17(int64_t t) { return (t * ((0xCA98 >> int(SR(t,9) & 14)) & 15)) | SR(t,8); }
inline int64_t n18(int64_t t) { return (t * ((SR(t,9) | SR(t,13)) & 15)) & 129; }
inline int64_t n19(int64_t t) { return (SR(t,13) | (i32(t) % 24)) & (SR(t,7) | (i32(t) % 19)); }
inline int64_t n20(int64_t t) { return (SR(t,6) & 1) ? SR(t,5) : SR(-t, 4); }
inline int64_t n21(int64_t t) { return t * ((SR(t,9) ^ (SR(t,9) - 1) ^ 1) % 13); }
inline int64_t n22(int64_t t) { return SR(t,5) | SL(t,4) | ((i32(t) & 1023) ^ 1981) | SR(t - 67, 4); }
inline int64_t n23(int64_t t) {
    return SR(~i32(t), 2) *
           (((127 & (t * (7 & SR(t,10)))) < (245 & (t * (2 + (5 & SR(t,14)))))) ? 1 : 0);
}
inline int64_t n24(int64_t t) {
    return SR(t,4) * (13 & int64_t(int32_t(0x8898a989u) >> int(SR(t,11) & 30)));
}

struct Node { const char* expr; NodeFn fn; const char* author; };

inline const Node* nodes() {
    static const Node N[25] = {
        {"t*(((t>>12)|(t>>8))&(63&(t>>4)))",                  n00, "viznut"},
        {"(t*(t>>5|t>>8))>>(t>>16)",                          n01, "tejeez"},
        {"t*(((t>>9)|(t>>13))&(25&(t>>6)))",                  n02, "visy"},
        {"t*(((t>>11)&(t>>8))&(123&(t>>3)))",                 n03, "tejeez"},
        {"t*(t>>((t>>9)|(t>>8))&(63&(t>>4)))",                n04, "visy \xC2\xB7 Invaders v Pong"},
        {"t*(42&t>>10)",                                      n05, "the 42 melody"},
        {"t&t>>8",                                            n06, "sierpinski"},
        {"(t>>6|t|t>>(t>>16))*10+((t>>11)&7)",                n07, "viznut"},
        {"(t|(t>>9|t>>7))*t&(t>>11|t>>9)",                    n08, "red-"},
        {"t*5&(t>>7)|t*3&(t*4>>10)",                          n09, "miiro"},
        {"(t>>7|t|t>>6)*10+4*(t&t>>13|t>>6)",                 n10, "viznut \xC2\xB7 xpansive+varjohukka"},
        {"((t*(t>>8|t>>9)&46&t>>8))^(t&t>>13|t>>6)",          n11, "xpansive \xC2\xB7 Lost in Space"},
        {"(t*5&t>>7)|(t*3&t>>10)",                            n12, "viznut"},
        {"(t*9&t>>4|t*5&t>>7|t*3&t/1024)-1",                  n13, "stephth"},
        {"t*(t>>9|t>>13)&16",                                 n14, "Niklas_Roy"},
        {"(t&t>>12)*(t>>4|t>>8)",                             n15, "krcko"},
        {"(t&t%255)-(t*3&t>>13&t>>6)",                        n16, "viznut"},
        {"t*(0xCA98>>(t>>9&14)&15)|t>>8",                     n17, "rrola"},
        {"(t*((t>>9|t>>13)&15))&129",                         n18, "Niklas_Roy"},
        {"(t>>13|t%24)&(t>>7|t%19)",                          n19, "robert"},
        {"t>>6&1?t>>5:-t>>4",                                 n20, "droid"},
        {"t*(((t>>9)^((t>>9)-1)^1)%13)",                      n21, "blueberry"},
        {"(t>>5)|(t<<4)|((t&1023)^1981)|((t-67)>>4)",         n22, "marmakoide \xC2\xB7 Lemmings March"},
        {"(~t>>2)*((127&t*(7&t>>10))<(245&t*(2+(5&t>>14))))", n23, "tejeez"},
        {"(t>>4)*(13&(0x8898a989>>(t>>11&30)))",              n24, "ryg"},
    };
    return N;
}

// The byte a node produces at `t`: the demo's clampByte, which is `(v|0) & 255`
// with NaN and Infinity (impossible here, kept for provenance) folded to 0.
inline int sample(int node, int64_t t) {
    return int(uint64_t(nodes()[node].fn(t)) & 255u);
}

} // namespace gritty
