#!/usr/bin/env python3
"""xrefs.py <target_hex>...: rel32 call/jmp sites in code sections that reach each target"""
import json, struct, sys
x = json.load(open('/home/claude/breakdown-recomp/analysis/xbe.json'))
d = open('/home/claude/breakdown-recomp/assets/default.xbe', 'rb').read()
ts = {int(a, 16) for a in sys.argv[1:]}
for s in x['sections']:
    if not s['exec']: continue
    b = d[s['raw']:s['raw'] + s['rsize']]
    for i in range(len(b) - 5):
        if b[i] in (0xE8, 0xE9):
            t = (s['va'] + i + 5 + struct.unpack_from('<i', b, i + 1)[0]) & 0xFFFFFFFF
            if t in ts: print(f"{t:08X} <- {s['va'] + i:08X} {s['name']} {'call' if b[i] == 0xE8 else 'jmp'}")
