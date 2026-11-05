// verify-expr — render expressions with expr.h, for comparison against node.
//
// Reads TSV on stdin: id <TAB> mode <TAB> code. Writes `id <TAB> values` or
// `id <TAB> !<reason>`. The companion verify-expr.mjs prints the same thing
// from a real JavaScript engine, and import-library.py keeps only the songs
// where the two agree. A song this evaluator renders differently is dropped,
// not shipped: a near-miss is somebody's music played wrong.
#include "../expr.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace gritty;

// `t` values spread across the first few minutes, since a song can be silent
// at the start and only differ much later.
static const long BASES[4] = {0, 8000, 100000, 1000000};

int main() {
    std::string line;
    char buf[1 << 20];
    while (std::fgets(buf, sizeof buf, stdin)) {
        line.assign(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        const size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        const size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        const std::string id   = line.substr(0, t1);
        const int         mode = std::atoi(line.substr(t1 + 1, t2 - t1 - 1).c_str());
        const std::string code = line.substr(t2 + 1);

        Expr e;
        if (!e.compile(code.c_str())) {
            std::printf("%s\t!%s\n", id.c_str(), e.error());
            continue;
        }
        std::string out;
        char n[32];
        for (int b = 0; b < 4; ++b)
            for (int i = 0; i < 64; ++i) {
                const double t = double(BASES[b] + i);
                if (mode == MODE_FLOAT) std::snprintf(n, sizeof n, "%.12g", double(exprByte(e, mode, t)));
                else                    std::snprintf(n, sizeof n, "%d", int(exprByte(e, mode, t)));
                if (!out.empty()) out += ',';
                out += n;
            }
        std::printf("%s\t%s\n", id.c_str(), out.c_str());
    }
    return 0;
}
