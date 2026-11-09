# mechassault-recomp: how much of the 2002 release's code is in the 2003 (Rev 1) one, and where the port's addresses moved.
# Both xbes are only read. Absolute addresses inside the code (any u32 in the image range) are masked before
# comparing, so code that only moved still matches; the masked values then give the data address map.
import re, struct, sys, collections

import os
OLD = os.environ.get("XBE_OLD", "game_files/MechAssault (USA, Europe) (En,Fr,De)/default.xbe")
NEW = os.environ.get("XBE_NEW", "game_files/MechAssault (USA) (En,Fr,De) (Rev 1)/default.xbe")

def load(path):
    x = open(path, "rb").read()
    r = lambda o: struct.unpack_from("<I", x, o)[0]
    base, n, h = r(0x104), r(0x11C), r(0x120) - r(0x104)
    secs = {}
    for i in range(n):
        o = h + i * 0x38
        name = x[r(o + 0x14) - base:][:12].split(b"\0")[0].decode()
        va, vs, raw, rs = r(o + 4), r(o + 8), r(o + 12), r(o + 16)
        secs.setdefault(name, (va, vs, raw, rs))
    img = bytearray(max(va + vs for va, vs, _, _ in secs.values()))
    for va, vs, raw, rs in secs.values():
        img[va:va + rs] = x[raw:raw + rs]
    return bytes(img), secs

def normalize(img, lo, hi):
    b = bytearray(img[lo:hi])
    masks = []
    i = 0
    while i + 4 <= len(b):
        v = struct.unpack_from("<I", b, i)[0]
        if 0x10000 <= v < 0x600000:
            b[i:i + 4] = b"\xAA\xAA\xAA\xAA"
            masks.append(i)
            i += 4
        else:
            i += 1
    return bytes(b), masks

old, osecs = load(OLD)
new, nsecs = load(NEW)
# all code sections together, as one range (they are contiguous from .text up to .rdata)
olo, ohi = osecs[".text"][0], osecs[".rdata"][0]
nlo, nhi = nsecs[".text"][0], nsecs[".rdata"][0]
on, _ = normalize(old, olo, ohi)
nn, _ = normalize(new, nlo, nhi)
print(f"code: 2002 {ohi-olo:#x} bytes, 2003 {nhi-nlo:#x} bytes")

W = 32
index = collections.defaultdict(list)
for i in range(0, len(nn) - W, 1):
    k = hash(nn[i:i + W])
    lst = index[k]
    if len(lst) < 3: lst.append(i)
found = sum(1 for i in range(0, len(on) - W, W) if hash(on[i:i + W]) in index)
total = (len(on) - W) // W
print(f"32-byte blocks of 2002 code found in 2003: {found}/{total} ({100*found/total:.1f}%)")
# per section
for name, (va, vs, _, _) in sorted(osecs.items(), key=lambda s: s[1][0]):
    if not (olo <= va < ohi): continue
    s0, s1 = va - olo, min(va + vs, ohi) - olo
    t = [hash(on[i:i + W]) in index for i in range(s0, s1 - W, W)]
    if t: print(f"  {name:9} {sum(t)}/{len(t)} ({100*sum(t)/len(t):.0f}%)")

def locate(addr, before=8, after=40):
    if not (olo + before <= addr < ohi - after): return None, "not in code"
    o = addr - olo
    for b, a in ((before, after), (0, after), (0, 24), (16, 16)):
        pat = on[o - b:o + a]
        hits = [m.start() for m in re.finditer(re.escape(pat), nn)] if len(pat) else []
        if len(hits) == 1: return nlo + hits[0] + b, f"unique ({b}+{a})"
        if len(hits) > 1: return None, f"{len(hits)} places"
    return None, "not found"

# data address map from matched code: the masked u32s side by side
pairs = collections.defaultdict(collections.Counter)
def collect(o_off, n_off, length):
    for j in range(0, length - 3):
        a = struct.unpack_from("<I", old, olo + o_off + j)[0]
        b = struct.unpack_from("<I", new, nlo + n_off + j)[0]
        if on[o_off + j:o_off + j + 4] == b"\xAA" * 4 and 0x10000 <= a < 0x600000 and 0x10000 <= b < 0x600000:
            pairs[a][b] += 1
for i in range(0, len(on) - W, W):
    hits = index.get(hash(on[i:i + W]))
    if hits and len(hits) == 1 and on[i:i + W] == nn[hits[0]:hits[0] + W]:
        collect(i, hits[0], W)

def mapdata(addr, span=0x200):
    # exact, else the nearest mapped neighbour within span (a field of the same object)
    if addr in pairs: return pairs[addr].most_common(1)[0][0], "exact"
    best = None
    for a in pairs:
        if abs(a - addr) <= span and (best is None or abs(a - addr) < abs(best - addr)): best = a
    if best is None: return None, "no reference matched"
    b = pairs[best].most_common(1)[0][0]
    return b + (addr - best), f"via {best:#x}"

