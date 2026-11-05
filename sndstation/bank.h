// bank.h — which 25 programs the map is currently made of.
//
// The map has always been 25 one-liners bilinearly blended. bytebeat.h holds
// the 25 it ships with, hand-ported and bit-exact. This is the layer that
// lets those 25 be somebody else's instead: a page of an imported library,
// compiled by expr.h and played through the same map, the same blend, the
// same destinations and the same free lanes.
//
// Two things a loaded song carries that a built-in does not:
//
//   * its own PLAYBACK MODE -- an unsigned byte, a signed byte, or a float.
//   * its own SAMPLE RATE. A song written for 44100 is a different piece of
//     music at 8000; it is not a slower version of itself, it is two and a
//     half octaves down with all its rhythm stretched. So each slot carries
//     the ratio between its native rate and the map's, and its `t` is scaled
//     by it. Four songs with four different native rates can sit on the same
//     map and each still be itself.
//
// Compiling happens on the UI thread, never in render(). A page change
// compiles into the bank the audio thread is NOT reading and then publishes
// it with one atomic store, so the audio thread never sees a half-built
// program and never waits for one.
#pragma once
#include "bytebeat.h"
#include "expr.h"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace gritty {

constexpr int LIB_MAX   = 4096;   // songs held from a pack
constexpr int LIB_SRC   = 1024;   // longest song source kept
constexpr int LIB_NAME  = 48;
constexpr int LIB_AUTH  = 40;

struct Song {
    char   name[LIB_NAME]{};
    char   author[LIB_AUTH]{};
    char   src[LIB_SRC]{};
    int    mode = MODE_BYTE;
    int    rate = 8000;
};

// One map slot: a built-in function, or a compiled song.
struct Slot {
    NodeFn  fn    = nullptr;
    Expr*   ex    = nullptr;
    int     mode  = MODE_BYTE;
    double  ratio = 1.0;
    char    name[LIB_NAME]{};
    char    author[LIB_AUTH]{};
};

class Bank {
public:
    Bank() { useBuiltins(); }

    // ── the 25 the port ships with ──
    void useBuiltins() {
        Slot* b = edit();
        for (int i = 0; i < 25; ++i) {
            b[i] = Slot{};
            b[i].fn = nodes()[i].fn;
            std::snprintf(b[i].name,   LIB_NAME, "%s", nodes()[i].expr);
            std::snprintf(b[i].author, LIB_AUTH, "%s", nodes()[i].author);
        }
        m_page = -1;
        publish();
    }

    // ── a pack written by tools/import-library.py ──
    // Returns the number of songs read, or -1 if the file could not be read.
    int load(const char* path) {
        std::FILE* f = std::fopen(path, "r");
        if (!f) return -1;
        static char line[LIB_SRC + 256];
        m_n = 0; m_long = 0;
        while (std::fgets(line, sizeof line, f)) {
            if (line[0] == '#') continue;
            size_t L = std::strlen(line);
            while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
            if (!L) continue;
            // name <TAB> author <TAB> mode <TAB> rate <TAB> code
            char* p = line;
            char* fld[5]; int nf = 0;
            fld[nf++] = p;
            for (; *p && nf < 5; ++p)
                if (*p == '\t') { *p = 0; fld[nf++] = p + 1; }
            if (nf < 5) continue;
            if (std::strlen(fld[4]) >= LIB_SRC) { ++m_long; continue; }
            if (m_n >= LIB_MAX) break;
            Song& s = m_song[m_n];
            std::snprintf(s.name,   LIB_NAME, "%s", fld[0]);
            std::snprintf(s.author, LIB_AUTH, "%s", fld[1]);
            s.mode = std::atoi(fld[2]);
            s.rate = std::atoi(fld[3]);
            if (s.rate < 1000 || s.rate > 192000) s.rate = 8000;
            std::snprintf(s.src, LIB_SRC, "%s", fld[4]);
            ++m_n;
        }
        std::fclose(f);
        return m_n;
    }

    int  songs()   const { return m_n; }
    int  pages()   const { return m_n > 0 ? (m_n + 24) / 25 : 0; }
    int  page()    const { return m_page; }
    int  tooLong() const { return m_long; }
    int  failed()  const { return m_failed; }

    // Fill the map with page `p` of the library. A song that will not compile
    // leaves its slot on the built-in it replaced, so the map is never silent
    // and you can always hear which slot did not take.
    void setPage(int p) {
        if (m_n <= 0) return;
        const int np = pages();
        p = ((p % np) + np) % np;
        Slot* b = edit();
        m_failed = 0;
        for (int i = 0; i < 25; ++i) {
            const int si = p * 25 + i;
            b[i] = Slot{};
            if (si >= m_n) {                       // a short last page
                b[i].fn = nodes()[i].fn;
                std::snprintf(b[i].name, LIB_NAME, "%s", "--");
                continue;
            }
            const Song& s = m_song[si];
            Expr* ex = &m_expr[m_write][i];
            if (ex->compile(s.src)) {
                b[i].ex    = ex;
                b[i].mode  = s.mode;
                b[i].ratio = double(s.rate) / double(BYTE_RATE_REF);
            } else {
                b[i].fn = nodes()[i].fn;           // keep the map playable
                ++m_failed;
            }
            std::snprintf(b[i].name,   LIB_NAME, "%s", s.name);
            std::snprintf(b[i].author, LIB_AUTH, "%s", s.author);
        }
        m_page = p;
        publish();
    }

    const Slot& slot(int i) const { return m_bank[m_read.load(std::memory_order_acquire)][i]; }

private:
    static constexpr int BYTE_RATE_REF = 8000;     // the map's own rate

    Song m_song[LIB_MAX];
    Slot m_bank[2][25];
    Expr m_expr[2][25];
    std::atomic<int> m_read{0};
    int  m_write = 1, m_n = 0, m_page = -1, m_long = 0, m_failed = 0;

    Slot* edit() { m_write = 1 - m_read.load(std::memory_order_relaxed); return m_bank[m_write]; }
    void  publish() { m_read.store(m_write, std::memory_order_release); }
};

inline Bank& bank() { static Bank b; return b; }

// ── the value of a map node at a map tick ──
// A float, not an int: a Floatbeat song has more than eight bits of detail
// and rounding it to a byte on the way past would be exactly the kind of
// quiet damage this port is not allowed to do.
inline float nodeValue(int n, int64_t t) {
    const Slot& s = bank().slot(n);
    if (s.fn) return float(int(uint64_t(s.fn(t)) & 255u));
    if (!s.ex) return 127.5f;
    const double tt = s.ratio == 1.0 ? double(t) : std::floor(double(t) * s.ratio);
    return exprByte(*s.ex, s.mode, tt);
}

} // namespace gritty
