#!/usr/bin/env python3
"""reconcile.py config/game.toml analysis/oovpa_found.json: xbe_symbols' [symbols]/[data] against the OOVPA scan.
SDK names map to XbSymbolDatabase names directly or through ALIAS."""
import json, re, sys
ALIAS = {'D3D_Device': None}
toml = open(sys.argv[1]).read()
ours = {}
for sec in ('symbols', 'data'):
    m = re.search(r'^\[%s\]\n(.*?)(?=^\[|\Z)' % sec, toml, re.S | re.M)
    for k, v in re.findall(r'^(\w+)\s*=\s*(0x[0-9A-Fa-f]+|0)\b', m.group(1), re.M): ours[k] = int(v, 16) if v != '0' else 0
theirs = json.load(open(sys.argv[2]))
agree, dis, fill, only = [], [], [], []
for k, v in sorted(ours.items()):
    t = theirs.get(k)
    if v and t: (agree if v == t else dis).append((k, v, t))
    elif not v and t: fill.append((k, t))
    elif v and not t: only.append((k, v))
missing = [k for k, v in ours.items() if not v and k not in theirs]
print(f'agree {len(agree)}, DISAGREE {len(dis)}, filled from the scan {len(fill)}, only xbe_symbols {len(only)}, neither {len(missing)}')
print('\n## Disagree'); [print(f'- {k}: xbe_symbols 0x{a:X}, scan 0x{b:X}') for k, a, b in dis]
print('\n## Filled from the scan (each needs checking)'); [print(f'- {k} = 0x{t:X}') for k, t in fill]
print('\n## Found by neither'); [print(f'- {k}') for k in sorted(missing)]
