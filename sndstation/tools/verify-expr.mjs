// verify-expr.mjs — the same rendering, from a real JavaScript engine.
//
// This is the reference. expr.h claims to implement JavaScript's integer
// semantics; the only way to know is to ask JavaScript. Reads the same TSV
// and prints the same format as verify-expr.cpp.
import { createInterface } from 'node:readline';

const BASES = [0, 8000, 100000, 1000000];

// The composer's own preamble, copied from its audio-processor: a song is
// compiled with every Math name in scope as a bare name, `int` as Math.trunc
// and `window` as the global. A song written `sin(t)` depends on this, so a
// reference run that leaves it out is testing the wrong language.
const MATH_HEADER = `var { ${
    Object.getOwnPropertyNames(Math).filter(k => k !== 'constructor').join(', ')
} } = Math; var int = Math.trunc; var window = globalThis;`;

// The composer's own byte conversions, mode for mode.
function toByte(v, mode) {
    if (mode === 2) {                       // Floatbeat
        if (Number.isNaN(v)) return 127.5;
        const c = v < -1 ? -1 : (v > 1 ? 1 : v);
        return (c + 1) * 127.5;
    }
    const i = v | 0;                        // ToInt32
    return ((mode === 1 ? i + 128 : i) & 255) >>> 0;
}

const rl = createInterface({ input: process.stdin, crlfDelay: Infinity });
for await (const line of rl) {
    const cut1 = line.indexOf('\t');
    if (cut1 < 0) continue;
    const cut2 = line.indexOf('\t', cut1 + 1);
    if (cut2 < 0) continue;
    const id = line.slice(0, cut1);
    const mode = Number(line.slice(cut1 + 1, cut2));
    const code = line.slice(cut2 + 1);

    let fn;
    try {
        fn = new Function('t', `${ MATH_HEADER }return (function(t) { return 0,\n${
            code || 0 }\n}).call(globalThis, t);`);
    } catch (err) {
        console.log(`${ id }\t!${ err.message }`);
        continue;
    }
    const out = [];
    try {
        for (const base of BASES)
            for (let i = 0; i < 64; ++i) {
                const v = fn(base + i);
                if (typeof v !== 'number') throw new TypeError('not a number');
                const b = toByte(v, mode);
                out.push(mode === 2 ? Number(b.toPrecision(12)) : b);
            }
    } catch (err) {
        console.log(`${ id }\t!${ err.message }`);
        continue;
    }
    console.log(`${ id }\t${ out.join(',') }`);
}
