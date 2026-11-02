#!/usr/bin/env python3
"""libcalls.py: XDK library functions (D3D, DSOUND, XMV, XGRPH, D3DX, XPP sections) the game's .text calls directly,
with the name XbSymbolDatabase or the config gives them. Unnamed ones are the identification priority."""
import json, struct, tomllib, collections
x = json.load(open('analysis/xbe.json')); d = open('assets/default.xbe', 'rb').read()
cfg = tomllib.load(open('config/game.toml', 'rb'))
names = {}
for n, v in {**cfg.get('symbols', {}), **cfg.get('data', {})}.items():
    if v: names.setdefault(v, set()).add(n)
for l in open('analysis/xbsdb/symbols.txt'):
    lib, k, n, a = l.split(); names.setdefault(int(a, 16), set()).add(n)
lib = {s['name']: s for s in x['sections'] if s['name'] in ('D3D', 'DSOUND', 'XMV', 'XGRPH', 'D3DX', 'XPP')}
def sec(a):
    for n, s in lib.items():
        if s['va'] <= a < s['va'] + s['vsize']: return n
t = [s for s in x['sections'] if s['name'] == '.text'][0]
b = d[t['raw']:t['raw'] + t['rsize']]
calls = collections.Counter()
for i in range(len(b) - 5):
    if b[i] == 0xE8:
        a = (t['va'] + i + 5 + struct.unpack_from('<i', b, i + 1)[0]) & 0xFFFFFFFF
        if sec(a): calls[a] += 1
rows = sorted(calls.items(), key=lambda kv: (sec(kv[0]), kv[0]))
un = [r for r in rows if r[0] not in names]
print(f'{len(rows)} library entry points called from .text, {len(un)} unnamed')
for a, c in rows:
    print(f"{sec(a):7} 0x{a:06X} x{c:<4} {' / '.join(sorted(names.get(a, {'?'})))}")
