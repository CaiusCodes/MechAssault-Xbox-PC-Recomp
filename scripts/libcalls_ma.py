#!/usr/bin/env python3
"""libcalls_ma.py: functions in the XDK library sections that game code (.text) calls directly and that have no
name in config (game.toml + mechassault.toml): candidates for missing HLE symbols. E8 rel32 scan, so a few false
hits are possible; each name still needs checking by disassembly."""
import re, struct, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from xdis import x, d, raw
funcs = set(int(a, 16) for a in re.findall(r'\{0x([0-9A-F]+)u, sub_', open('generated/game/x86_func_mapping.cpp').read()))
named = {}
for f in ('config/game.toml', 'config/mechassault.toml'):
    for k, v in re.findall(r'^(\w+)\s*=\s*(0x[0-9A-Fa-f]+)', open(f).read(), re.M): named[int(v, 16)] = k
libsecs = [s for s in x['sections'] if s['name'] not in ('.text', '.rdata', '.data', '.data1', 'XON_RD', 'DOLBY', 'BINKDATA', '$$XTIMAGE', '$$XSIMAGE')]
def sec_of(va):
    for s in x['sections']:
        if s['va'] <= va < s['va'] + s['vsize']: return s['name']
callers = {}
for s in x['sections']:
    if s['name'] not in ('.text',): continue
    data = d[s['raw']:s['raw'] + s['rsize']]
    for m in re.finditer(b'\xE8', data):
        o = m.start()
        if o + 5 > len(data): break
        t = (s['va'] + o + 5 + struct.unpack_from('<i', data, o + 1)[0]) & 0xFFFFFFFF
        if t in funcs and sec_of(t) not in (None, '.text'):
            callers.setdefault(t, []).append(s['va'] + o)
want = sys.argv[1] if len(sys.argv) > 1 else None
for t in sorted(callers):
    sec = sec_of(t)
    if want and sec != want: continue
    print(f'{t:08X} {sec:8} {named.get(t, "?"):45} {len(callers[t]):4} calls, e.g. {callers[t][0]:08X}')
