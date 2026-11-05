#!/usr/bin/env python3
"""Import a bytebeat library into a pack this port can play.

Usage: tools/import-library.py <all.gz> [-o library.pack]

The input is the song index published by bytebeat-composer
(https://github.com/SthephanShinkufag/bytebeat-composer), as served at
data/library/all.gz. Download your own copy; nothing from it is committed
here. The songs in it are two thousand separate compositions by a couple of
hundred named people, published by each of them with their own name attached,
so they are theirs to distribute and not ours to vendor. The pack this writes
is a local artifact, gitignored, carrying every author and title through to
the screen so the port never shows you somebody's work unattributed.

Every song is rendered twice -- once by expr.h and once by node, the engine
these programs were actually written for -- and kept only where the two agree
exactly. A song this port would play even slightly differently is dropped. A
near-miss is not a near-miss, it is somebody's music played wrong.
"""
import argparse, gzip, json, re, subprocess, sys, pathlib

HERE = pathlib.Path(__file__).resolve().parent
MODES = {None: 0, 'Bytebeat': 0, 'Signed Bytebeat': 1, 'Floatbeat': 2}

def strip_js(code):
    """One line, no comments. Expressions only; a statement body will fail
    the parse in both evaluators and be dropped, which is the point."""
    code = re.sub(r'/\*.*?\*/', ' ', code, flags=re.S)
    code = re.sub(r'(?m)//[^\n]*$', '', code)
    return ' '.join(code.split()).replace('\t', ' ')

def render(cmd, rows, label):
    payload = ''.join(f'{i}\t{m}\t{c}\n' for i, m, c in rows)
    out = subprocess.run(cmd, input=payload, capture_output=True, text=True, cwd=HERE.parent)
    if out.returncode != 0:
        sys.exit(f'{label} failed: {out.stderr[:400]}')
    got = {}
    for line in out.stdout.splitlines():
        if '\t' in line:
            k, v = line.split('\t', 1)
            got[k] = v
    return got

def same(a, b, mode):
    """Integer modes must agree exactly. Float mode is allowed 1e-9 of a byte,
    because libm and V8 do not agree to the last bit on sin, cos or pow -- a
    difference nothing can hear, and not one this port introduced."""
    if a == b:
        return True
    if mode != 2:
        return False
    xs, ys = a.split(','), b.split(',')
    if len(xs) != len(ys):
        return False
    return all(abs(float(x) - float(y)) <= 1e-9 for x, y in zip(xs, ys))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('index', type=pathlib.Path)
    ap.add_argument('-o', '--out', type=pathlib.Path, default=pathlib.Path('library.pack'))
    args = ap.parse_args()

    blocks = json.load(gzip.open(args.index))
    rows, meta = [], {}
    skipped = {'no inline code': 0, 'funcbeat': 0, 'truncated': 0, 'stereo': 0}
    for block in blocks:
        author = block.get('author') or 'unknown'
        for song in block.get('songs', []):
            code = song.get('code')
            if not code:
                skipped['no inline code'] += 1; continue
            if song.get('isTruncated'):
                skipped['truncated'] += 1; continue
            mode = song.get('mode')
            if mode == 'Funcbeat':
                skipped['funcbeat'] += 1; continue
            if song.get('stereo'):
                skipped['stereo'] += 1; continue
            sid = str(len(rows))
            rows.append((sid, MODES.get(mode, 0), strip_js(code)))
            meta[sid] = (song.get('name') or 'untitled', author,
                         MODES.get(mode, 0), int(song.get('sampleRate') or 8000))

    print(f'{len(blocks)} authors, {len(rows)} songs with inline code to check')
    for k, v in skipped.items():
        print(f'  skipped {v} ({k})')

    cpp = render(['c++', '-std=c++20', '-O2', '-o', '/tmp/verify-expr',
                  'tools/verify-expr.cpp'], [], 'compile')
    mine = render(['/tmp/verify-expr'], rows, 'verify-expr')
    theirs = render(['node', 'tools/verify-expr.mjs'], rows, 'node')

    kept, rejected = [], {}
    for sid, mode, code in rows:
        a, b = mine.get(sid, '!missing'), theirs.get(sid, '!missing')
        if b.startswith('!'):
            rejected[sid] = f'javascript itself refuses it: {b[1:80]}'; continue
        if a.startswith('!'):
            rejected[sid] = f'this evaluator cannot read it: {a[1:80]}'; continue
        if not same(a, b, mode):
            rejected[sid] = 'renders differently from javascript'; continue
        kept.append((sid, code))

    with open(args.out, 'w') as f:
        f.write(f'# gritty-library 1 {len(kept)}\n')
        for sid, code in kept:
            name, author, mode, rate = meta[sid]
            name = name.replace('\t', ' ')[:47]
            author = author.replace('\t', ' ')[:39]
            f.write(f'{name}\t{author}\t{mode}\t{rate}\t{code}\n')

    why = {}
    for r in rejected.values():
        why[r.split(':')[0]] = why.get(r.split(':')[0], 0) + 1
    print(f'\nkept {len(kept)} of {len(rows)} -> {args.out}')
    for k, v in sorted(why.items(), key=lambda kv: -kv[1]):
        print(f'  dropped {v}: {k}')

if __name__ == '__main__':
    main()
