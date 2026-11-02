#!/usr/bin/env python3
"""Find game variables by comparing harness memory dumps (MEMDUMP=...): prints float32 addresses whose changes
between consecutive dumps match a pattern. Pattern: one letter per step, 'c' changed, 's' same, '+'/'-' increased /
decreased, '*' any. --type picks the value type (f4 float, i4, i2, u2), --same the tolerance of 's' (default
--min-delta), --wrap a period for angles (6.2831853 for radians, 65536 for 16-bit angles). Example: memdiff.py cap/mem_6000.bin cap/mem_6200.bin cap/mem_6400.bin --pattern ++"""
import struct, sys, argparse, math
import numpy as np

def load(path):
    regions = {}
    with open(path, 'rb') as f:
        while True:
            h = f.read(8)
            if len(h) < 8: break
            va, size = struct.unpack('<II', h)
            regions[va] = np.frombuffer(f.read(size), dtype=np.uint8)
    return regions

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dumps', nargs='+')
    ap.add_argument('--pattern', required=True)
    ap.add_argument('--min-delta', type=float, default=1e-4)
    ap.add_argument('--max-abs', type=float, default=1e5)
    ap.add_argument('--limit', type=int, default=60)
    ap.add_argument('--below', type=lambda x: int(x, 0), default=0xFFFFFFFF)
    ap.add_argument('--type', default='f4', choices=['f4', 'i4', 'i2', 'u2'])
    ap.add_argument('--same', type=float, default=None)
    ap.add_argument('--wrap', type=float, default=0)
    a = ap.parse_args()
    same = a.min_delta if a.same is None else a.same
    ds = [load(p) for p in a.dumps]
    assert len(a.pattern) == len(ds) - 1
    hits = []
    for va, buf in ds[0].items():
        if va >= a.below: continue
        if any(va not in d or len(d[va]) != len(buf) for d in ds[1:]): continue
        size = int(a.type[1])
        n = len(buf) // size
        vals = [np.frombuffer(d[va][:n * size].tobytes(), dtype='<' + a.type).astype(np.float64) for d in ds]
        ok = np.ones(n, dtype=bool)
        for v in vals:
            ok &= np.isfinite(v) & (np.abs(v) < a.max_abs)
        for i, p in enumerate(a.pattern):
            d = vals[i + 1] - vals[i]
            if a.wrap: d = (d + a.wrap / 2) % a.wrap - a.wrap / 2
            if p == 'c': ok &= np.abs(d) > a.min_delta
            elif p == 's': ok &= np.abs(d) <= same
            elif p == '+': ok &= d > a.min_delta
            elif p == '-': ok &= d < -a.min_delta
        for j in np.nonzero(ok)[0]:
            hits.append((va + size * int(j), [float(v[j]) for v in vals]))
    print(f'{len(hits)} matches')
    for addr, v in hits[:a.limit]:
        print(f'{addr:08X} ' + ' '.join(f'{x: .5f}' for x in v))

main()
