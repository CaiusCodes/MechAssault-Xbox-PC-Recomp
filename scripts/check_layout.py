#!/usr/bin/env python3
"""check_layout.py <layout.toml>: for each layout value, find the function its comment names in Breakdown (config or
XbSymbolDatabase) and look for the value as an operand there. Prints VERIFIED / NOT FOUND / NO FUNCTION per key."""
import re, sys, json, subprocess, tomllib
x = json.load(open('analysis/xbe.json')); d = open('assets/default.xbe', 'rb').read()
cfg = tomllib.load(open('config/game.toml', 'rb'))
addr = {n: v for n, v in {**cfg['symbols'], **cfg.get('data', {})}.items() if v}
for l in open('analysis/xbsdb/symbols.txt'):
    lib, k, n, a = l.split(); addr.setdefault(re.sub(r'_\d+$', '', n), int(a, 16))
def body(va, n=0x600):
    for s in x['sections']:
        if s['va'] <= va < s['va'] + s['rsize']:
            raw = va - s['va'] + s['raw']; b = d[raw:raw + n]
            i = b.find(b'\xcc\xcc\xcc'); b = b[:i if i > 0 else n]
            open('/tmp/f.bin', 'wb').write(b)
            return subprocess.run(['objdump', '-D', '-b', 'binary', '-mi386', '-Mintel', f'--adjust-vma={va:#x}', '/tmp/f.bin'],
                                  capture_output=True, text=True).stdout
for line in open(sys.argv[1]):
    m = re.match(r'(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*#\s*(.*)', line)
    if not m: continue
    key, val, why = m.group(1), int(m.group(2), 0), m.group(3)
    fns = [f for f in re.findall(r'\b((?:D3DDevice|D3D|Direct3D|CDirectSound\w*|IDirectSound\w*|XGetDevices|XGetDeviceChanges)_?\w*)', why) if f in addr]
    if not fns: print(f'NO FUNCTION  {key} = {val:#x}  ({why[:70]})'); continue
    hit = []
    for f in fns:
        t = body(addr[f]) or ''
        if re.search(rf'(?<![0-9a-fx]){val:#x}\b', t) or (val == 0 and re.search(r'\[(e[a-z]{2})\]', t)): hit.append(f)
    print(f"{'VERIFIED    ' if hit else 'NOT FOUND   '} {key} = {val:#x}  in {', '.join(hit or fns)}")
